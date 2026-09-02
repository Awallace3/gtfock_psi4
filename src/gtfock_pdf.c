/*
 * Distributed density-fitted J and K. See docs/distributed-df.md for the
 * design; the phase names used in the comments below are the ones that note
 * defines.
 */

#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <mpi.h>
#include <omp.h>

#include "gtfock_df.h"
#include "gtfock_pdf.h"

/*
 * LP64 Fortran BLAS/LAPACK, matching the gtf_numeric stack: 32-bit integers
 * and trailing-underscore symbols. Declared here rather than pulled in through
 * <mkl.h> so this translation unit builds unchanged against either MKL or the
 * netlib reference libraries.
 */
extern void dgemm_(const char *transa, const char *transb, const int *m,
                   const int *n, const int *k, const double *alpha,
                   const double *a, const int *lda, const double *b,
                   const int *ldb, const double *beta, double *c,
                   const int *ldc);
extern void dgemv_(const char *trans, const int *m, const int *n,
                   const double *alpha, const double *a, const int *lda,
                   const double *x, const int *incx, const double *beta,
                   double *y, const int *incy);
extern void dsyrk_(const char *uplo, const char *trans, const int *n,
                   const int *k, const double *alpha, const double *a,
                   const int *lda, const double *beta, double *c,
                   const int *ldc);
extern void dtrsm_(const char *side, const char *uplo, const char *transa,
                   const char *diag, const int *m, const int *n,
                   const double *alpha, const double *a, const int *lda,
                   double *b, const int *ldb);
extern void dpstrf_(const char *uplo, const int *n, double *a, const int *lda,
                    int *piv, int *rank, const double *tol, double *work,
                    int *info);

#define PDF_DEFAULT_COND 1e-12

/*
 * Auxiliary functions whose half-transforms share one dsyrk, as a byte budget
 * on the nbf x q_batch x nocc buffer plus a hard cap. Batching amortises the
 * pass over the nbf x nbf exchange accumulator; past a handful of auxiliary
 * functions that pass is no longer what the loop is waiting for, and the
 * buffer is pure overhead.
 */
#define PDF_K_BATCH_BYTES ((size_t) 32 * 1024 * 1024)
#define PDF_K_MAX_BATCH 16

/*
 * AO rows in one thread's band of densified B_Q rows. This is the exchange
 * half-transform's blocking, and both ends of the range cost. Too few rows and
 * the GEMM behind the band is too narrow to amortise its own setup: at one
 * shell per band, two or three rows wide, the build ran an order of magnitude
 * slower than the dense scatter this replaced. Too many and the band stops
 * fitting in L2 alongside Cocc, which puts the densified rows back out to
 * memory and gives up the whole point of banding.
 *
 * Measured on a 24-core socket with 512 KiB of L2 per core, the cost is flat
 * from about 16 to 48 rows on both a 574- and a 260-function case, and rises on
 * either side; 32 is the middle of that plateau. A band is 32 * nbf doubles per
 * thread, so the scratch stays small and, unlike the nbf^2 slice it replaced,
 * does not grow with the system.
 */
#define PDF_PANEL_ROWS 32

struct PDF
{
    MPI_Comm comm;
    int nranks, rank, nthreads;

    GTFDF_t df;
    int nbf, naux, nsh_p, nsh_a;
    /* Largest primary shell dimension. */
    int max_dim;
    /* Rows the metric factorization retained, and the ones it dropped. */
    int nfit, nnull;

    /* Wall seconds per PDF_create phase; zeroed by the calloc in PDF_create. */
    double phase_s[PDF_NPHASES];
    /* Wall seconds per PDF_computeJK part, accumulated over calls, and the
     * number of calls that got as far as the reduction. */
    double jk_phase_s[PDF_NJKPHASES];
    int jk_calls;

    /* Unique primary shell pairs (M >= N), ordered by M then N, so the block
     * for a pair sits at index M * (M + 1) / 2 + N. */
    int npair;
    int *pair_m, *pair_n;
    /* AO-element offset of each pair's block, npair + 1 entries. */
    size_t *pair_off;
    size_t npair_ao;

    /* Phase A/B ownership: a contiguous run of shell pairs per rank. */
    int *pair_first;   /* nranks + 1 */
    int *ao_counts;    /* nranks */
    int *ao_displs;    /* nranks */
    int ao_local;

    /* Post-Phase-C ownership: a contiguous run of fitted tensor rows. */
    int *q_counts;     /* nranks */
    int *q_displs;     /* nranks + 1 */
    int q_local;

    /* q_local x npair_ao, row-major: the fitted tensor this rank owns. */
    double *B;

    /* Per-iteration scratch, allocated once. */
    double *dvec;      /* npair_ao */
    double *cvec;      /* q_local */
    double *jkbuf;     /* 2 * nbf * nbf: J then K */
    /*
     * Runs of consecutive primary shells, each covering a contiguous band of
     * AO rows no wider than panel_rows. One band of one B_Q is what a thread
     * densifies and half-transforms at a time.
     */
    int ngroups;
    int *group_first;  /* ngroups + 1 shell indices */
    int *group_row;    /* ngroups + 1 AO row starts, last one nbf */
    int panel_rows;
    /* nthreads * panel_rows * nbf. */
    double *panels;
    /* nbf * q_batch * nocc, sized on first use and grown as nocc grows. */
    double *half;
    size_t half_cap;
};

/* ---------------------------------------------------------------- setup -- */

static void pdf_free(PDF_t p)
{
    if (p == NULL) return;
    free(p->pair_m);
    free(p->pair_n);
    free(p->pair_off);
    free(p->pair_first);
    free(p->group_first);
    free(p->group_row);
    free(p->ao_counts);
    free(p->ao_displs);
    free(p->q_counts);
    free(p->q_displs);
    free(p->B);
    free(p->dvec);
    free(p->cvec);
    free(p->jkbuf);
    free(p->panels);
    free(p->half);
    if (p->df != NULL) GTFDF_destroy(p->df);
    if (p->comm != MPI_COMM_NULL) MPI_Comm_free(&p->comm);
    free(p);
}

/*
 * Split the shell-pair list into contiguous runs of near-equal AO-element
 * count. Balancing on elements rather than pairs matters because a (d,d) pair
 * carries 36 elements against an (s,s) pair's one.
 */
static void pdf_partition_pairs(PDF_t p)
{
    p->pair_first[0] = 0;
    for (int r = 1; r < p->nranks; r++)
    {
        size_t target = p->npair_ao / p->nranks * r
                      + p->npair_ao % p->nranks * r / p->nranks;
        int i = p->pair_first[r - 1];
        while (i < p->npair && p->pair_off[i] < target) i++;
        p->pair_first[r] = i;
    }
    p->pair_first[p->nranks] = p->npair;

    for (int r = 0; r < p->nranks; r++)
    {
        p->ao_displs[r] = (int) p->pair_off[p->pair_first[r]];
        p->ao_counts[r] = (int) (p->pair_off[p->pair_first[r + 1]]
                                 - p->pair_off[p->pair_first[r]]);
    }
    p->ao_local = p->ao_counts[p->rank];
}

/*
 * Split the fitted axis. Called after the metric factorization, because the
 * rows that survive it are what actually get distributed.
 */
static void pdf_partition_aux(PDF_t p)
{
    int base = p->nfit / p->nranks;
    int rem  = p->nfit % p->nranks;
    p->q_displs[0] = 0;
    for (int r = 0; r < p->nranks; r++)
    {
        p->q_counts[r] = base + (r < rem ? 1 : 0);
        p->q_displs[r + 1] = p->q_displs[r] + p->q_counts[r];
    }
    p->q_local = p->q_counts[p->rank];
}

/*
 * Replicated Coulomb metric (P|Q). Auxiliary shell pairs are dealt out
 * round-robin and the result is reduced, so every rank leaves with the same
 * bits and the factorization below needs no further agreement.
 */
static CIntStatus_t pdf_build_metric(PDF_t p, double *metric)
{
    const int nsh_a = p->nsh_a;
    const int naux = p->naux;
    CIntStatus_t status = CINT_STATUS_SUCCESS;

    memset(metric, 0, sizeof(double) * (size_t) naux * naux);

    int npq = nsh_a * (nsh_a + 1) / 2;
#pragma omp parallel num_threads(p->nthreads)
    {
        int tid = omp_get_thread_num();
#pragma omp for schedule(dynamic)
        for (int idx = 0; idx < npq; idx++)
        {
            if (idx % p->nranks != p->rank) continue;
            /* Unrank idx into P >= Q without a table. */
            int P = (int) ((sqrt(8.0 * idx + 1.0) - 1.0) * 0.5);
            while ((P + 1) * (P + 2) / 2 <= idx) P++;
            while (P * (P + 1) / 2 > idx) P--;
            int Q = idx - P * (P + 1) / 2;

            double *ints = NULL;
            int nints = 0;
            CIntStatus_t s = GTFDF_compute2c(p->df, tid, P, Q, &ints, &nints);
            if (s != CINT_STATUS_SUCCESS)
            {
#pragma omp atomic write
                status = (int) s;
                continue;
            }
            if (nints == 0) continue;

            int dP = GTFDF_auxShellDim(p->df, P);
            int dQ = GTFDF_auxShellDim(p->df, Q);
            int sP = GTFDF_auxFuncStart(p->df, P);
            int sQ = GTFDF_auxFuncStart(p->df, Q);
            for (int a = 0; a < dP; a++)
            {
                for (int b = 0; b < dQ; b++)
                {
                    double v = ints[a * dQ + b];
                    metric[(size_t) (sP + a) * naux + (sQ + b)] = v;
                    metric[(size_t) (sQ + b) * naux + (sP + a)] = v;
                }
            }
        }
    }
    if (status != CINT_STATUS_SUCCESS) return (CIntStatus_t) status;

    if (MPI_Allreduce(MPI_IN_PLACE, metric, naux * naux, MPI_DOUBLE, MPI_SUM,
                      p->comm) != MPI_SUCCESS)
        return CINT_STATUS_EXECUTION_FAILED;
    return CINT_STATUS_SUCCESS;
}

/*
 * Factor the Coulomb metric in place: P^T J P = L L^T, pivoted and truncated.
 *
 * The first version of this formed J^-1/2 with a symmetric eigendecomposition.
 * Nothing downstream needs the inverse square root itself. Both J and K see the
 * fitted tensor only through B^T B = A^T J^-1 A, so any factor of J^-1 gives
 * the same matrices, and a Cholesky costs n^3/3 flops against dsyev's ~9 n^3
 * plus a dsyrk. It also runs in place, where the eigen path held the
 * eigenvectors and the assembled J^-1/2 at once, and it turns Phase B from a
 * full GEMM into a triangular solve of half the flops that can overwrite its
 * input. This matters more than the flop count suggests: the factorization is
 * replicated bit-for-bit on every rank, so it is the one part of setup that
 * gets no faster as ranks are added, and at a fixed core count it gets slower.
 *
 * Truncation is a different criterion from the eigenvalue floor it replaces,
 * and `cond` accordingly means something different. dpstrf pivots the largest
 * remaining diagonal of the Schur complement to the front and stops once that
 * falls below `cond` times the first; the retained leading block is then
 * factored exactly, so the result is an exact fit in a pivot-selected subset of
 * the auxiliary basis rather than a pseudo-inverse over all of it. The eigen
 * path instead dropped every eigenvector below `cond` times the largest
 * eigenvalue, which is also what Psi4's DFHelper does with
 * DF_FITTING_CONDITION.
 *
 * The two criteria select the same functions on a metric that is clearly
 * well conditioned or clearly rank-deficient, and disagree when its spectrum
 * straddles the cutoff -- where they disagree, this one keeps more. Water in
 * cc-pVDZ/cc-pVDZ-JKFIT at Psi4's default 1e-10 is exactly that case: the
 * smallest eigenvalue is 3.83e-08 against a floor of 4.17e-08, so DFHelper
 * discards one vector and dpstrf keeps all 131. Measured against the
 * untruncated fit A^T J^-1 A, the resulting Coulomb metric contraction is off
 * by 8.0e-07 for the eigen path and 1.6e-14 for this one, and J differs
 * between the two engines by 3.4e-07. Below 1e-12, where neither truncates,
 * they agree to 6.9e-12. tests/pytests/test_gtfock.py pins the condition for
 * its tight MemDFJK comparisons for that reason and tests the truncation
 * difference separately. PDF_nMetricNullVectors reports what was dropped.
 *
 * On return the leading nfit x nfit Fortran lower triangle of `metric` is L,
 * and `*piv_out` is LAPACK's one-based permutation: factor row k carries
 * auxiliary function piv[k] - 1.
 */
static CIntStatus_t pdf_factor_metric(PDF_t p, double *metric, double cond,
                                      int **piv_out)
{
    const int n = p->naux;
    int *piv = (int *) malloc(sizeof(int) * (size_t) (n > 0 ? n : 1));
    double *work =
        (double *) malloc(sizeof(double) * 2 * (size_t) (n > 0 ? n : 1));
    if (piv == NULL || work == NULL)
    {
        free(piv);
        free(work);
        return CINT_STATUS_ALLOC_FAILED;
    }

    /*
     * dpstrf's own default is n * eps * max(diag). Passing an explicit multiple
     * of the largest diagonal keeps `fitting_cond` meaning the same relative
     * thing it meant as an eigenvalue floor, and keeps the cutoff independent
     * of the auxiliary basis size.
     */
    double dmax = 0.0;
    for (int k = 0; k < n; k++)
        if (metric[(size_t) k * n + k] > dmax) dmax = metric[(size_t) k * n + k];
    double tol = cond * dmax;

    int rank = 0, info = 0;
    dpstrf_("L", &n, metric, &n, piv, &rank, &tol, work, &info);
    free(work);
    /* info > 0 only says the tolerance stopped the factorization early, which
     * is the rank-deficient case this is written for; `rank` is then the
     * retained order. info < 0 is a bad argument. */
    if (info < 0 || rank <= 0 || rank > n)
    {
        free(piv);
        return CINT_STATUS_EXECUTION_FAILED;
    }

    p->nfit = rank;
    p->nnull = n - rank;
    *piv_out = piv;
    return CINT_STATUS_SUCCESS;
}

/*
 * Reorder the auxiliary axis of this rank's three-center block into pivot
 * order, in place. An auxiliary function's whole row is contiguous here, so
 * following the permutation's cycles turns this into a sequence of full-row
 * memcpys; LAPACK's dlapmt would do the same permutation an element at a time.
 */
static CIntStatus_t pdf_permute_aux_rows(PDF_t p, double *A3, const int *piv)
{
    const size_t m = (size_t) p->ao_local;
    const int n = p->naux;
    if (m == 0 || A3 == NULL) return CINT_STATUS_SUCCESS;

    char *seen = (char *) calloc((size_t) n, 1);
    double *tmp = (double *) malloc(sizeof(double) * m);
    if (seen == NULL || tmp == NULL)
    {
        free(seen);
        free(tmp);
        return CINT_STATUS_ALLOC_FAILED;
    }

    for (int s0 = 0; s0 < n; s0++)
    {
        if (seen[s0]) continue;
        seen[s0] = 1;
        if (piv[s0] - 1 == s0) continue;
        /* Lift the cycle's first row out, then pull each successor into the
         * hole it leaves behind. */
        memcpy(tmp, &A3[(size_t) s0 * m], sizeof(double) * m);
        int j = s0;
        for (;;)
        {
            int k = piv[j] - 1;
            if (k == s0)
            {
                memcpy(&A3[(size_t) j * m], tmp, sizeof(double) * m);
                break;
            }
            memcpy(&A3[(size_t) j * m], &A3[(size_t) k * m], sizeof(double) * m);
            seen[k] = 1;
            j = k;
        }
    }
    free(seen);
    free(tmp);
    return CINT_STATUS_SUCCESS;
}

/* Phase A: three-center integrals over this rank's shell pairs, no comms. */
static CIntStatus_t pdf_build_3c(PDF_t p, double *A3)
{
    const int nsh_a = p->nsh_a;
    const int ao_local = p->ao_local;
    const int pfirst = p->pair_first[p->rank];
    const int plast = p->pair_first[p->rank + 1];
    /* Shared across the OpenMP region, so a plain int for `atomic write`. */
    int status = CINT_STATUS_SUCCESS;

    if (ao_local == 0) return CINT_STATUS_SUCCESS;
    memset(A3, 0, sizeof(double) * (size_t) p->naux * ao_local);

#pragma omp parallel num_threads(p->nthreads)
    {
        int tid = omp_get_thread_num();
#pragma omp for schedule(dynamic)
        for (int ip = pfirst; ip < plast; ip++)
        {
            int M = p->pair_m[ip];
            int N = p->pair_n[ip];
            int dM = GTFDF_priShellDim(p->df, M);
            int dN = GTFDF_priShellDim(p->df, N);
            size_t base = p->pair_off[ip] - (size_t) p->ao_displs[p->rank];

            for (int P = 0; P < nsh_a; P++)
            {
                double *ints = NULL;
                int nints = 0;
                CIntStatus_t s =
                    GTFDF_compute3c(p->df, tid, P, M, N, &ints, &nints);
                if (s != CINT_STATUS_SUCCESS)
                {
#pragma omp atomic write
                    status = (int) s;
                    continue;
                }
                if (nints == 0) continue;  /* screened: A3 stays zero */

                int dP = GTFDF_auxShellDim(p->df, P);
                int sP = GTFDF_auxFuncStart(p->df, P);
                for (int a = 0; a < dP; a++)
                {
                    double *dst = &A3[(size_t) (sP + a) * ao_local + base];
                    const double *src = &ints[(size_t) a * dM * dN];
                    memcpy(dst, src, sizeof(double) * (size_t) dM * dN);
                }
            }
        }
    }
    return (CIntStatus_t) status;
}

/*
 * Phase C: go from "all Q, my mn" to "my Q, all mn". `fit` is the rank's
 * fitted block, still keyed by every Q; at one rank PDF_create skips this
 * entirely and adopts that buffer instead of copying it.
 *
 * MPI_Alltoallw would express this in one call but its displacements are byte
 * counts in int, which overflow well below the 24 GB/rank this is sized for.
 * Phased pairwise Sendrecv with a strided receive type has the same message
 * count, needs no third copy, and has no such ceiling.
 */
static CIntStatus_t pdf_redistribute(PDF_t p, const double *fit)
{
    const int npair_ao = (int) p->npair_ao;
    const int ao_local = p->ao_local;

    for (int q = 0; q < p->q_local; q++)
        memcpy(&p->B[(size_t) q * npair_ao + p->ao_displs[p->rank]],
               &fit[(size_t) (p->q_displs[p->rank] + q) * ao_local],
               sizeof(double) * (size_t) ao_local);

    MPI_Datatype rowtype;
    MPI_Type_contiguous(ao_local, MPI_DOUBLE, &rowtype);
    MPI_Type_commit(&rowtype);

    for (int k = 1; k < p->nranks; k++)
    {
        int dst = (p->rank + k) % p->nranks;
        int src = (p->rank - k + p->nranks) % p->nranks;

        /* p->B is always allocated, so it stands in as a valid zero-count
         * address rather than passing MPI a null pointer. */
        const double *sbuf = (ao_local > 0)
            ? &fit[(size_t) p->q_displs[dst] * ao_local] : p->B;

        MPI_Datatype blocktype;
        MPI_Type_vector(p->q_local, p->ao_counts[src], npair_ao, MPI_DOUBLE,
                        &blocktype);
        MPI_Type_commit(&blocktype);
        double *rbuf = (p->q_local > 0) ? &p->B[p->ao_displs[src]] : p->B;

        int rc = MPI_Sendrecv(sbuf, p->q_counts[dst], rowtype, dst, 0,
                              rbuf, 1, blocktype, src, 0, p->comm,
                              MPI_STATUS_IGNORE);
        MPI_Type_free(&blocktype);
        if (rc != MPI_SUCCESS)
        {
            MPI_Type_free(&rowtype);
            return CINT_STATUS_EXECUTION_FAILED;
        }
    }
    MPI_Type_free(&rowtype);
    return CINT_STATUS_SUCCESS;
}

CIntStatus_t PDF_create(MPI_Comm comm, BasisSet_t primary, BasisSet_t auxiliary,
                        int nthreads, double fitting_cond, PDF_t *pdf_out)
{
    if (pdf_out == NULL || primary == NULL || auxiliary == NULL)
        return CINT_STATUS_INVALID_VALUE;
    *pdf_out = NULL;

    PDF_t p = (PDF_t) calloc(1, sizeof(struct PDF));
    if (p == NULL) return CINT_STATUS_ALLOC_FAILED;
    p->comm = MPI_COMM_NULL;

    if (MPI_Comm_dup(comm, &p->comm) != MPI_SUCCESS)
    {
        free(p);
        return CINT_STATUS_EXECUTION_FAILED;
    }
    MPI_Comm_size(p->comm, &p->nranks);
    MPI_Comm_rank(p->comm, &p->rank);
    p->nthreads = nthreads > 0 ? nthreads : omp_get_max_threads();

    CIntStatus_t status = GTFDF_create(primary, auxiliary, p->nthreads, &p->df);
    if (status != CINT_STATUS_SUCCESS)
    {
        pdf_free(p);
        return status;
    }

    p->nbf = GTFDF_nPriFuncs(p->df);
    p->naux = GTFDF_nAuxFuncs(p->df);
    p->nsh_p = GTFDF_nPriShells(p->df);
    p->nsh_a = GTFDF_nAuxShells(p->df);
    p->npair = p->nsh_p * (p->nsh_p + 1) / 2;
    p->max_dim = 1;
    for (int sh = 0; sh < p->nsh_p; sh++)
    {
        int d = GTFDF_priShellDim(p->df, sh);
        if (d > p->max_dim) p->max_dim = d;
    }
    p->panel_rows = PDF_PANEL_ROWS;
    if (p->panel_rows < p->max_dim) p->panel_rows = p->max_dim;
    if (p->panel_rows > p->nbf) p->panel_rows = p->nbf;

    p->pair_m = (int *) malloc(sizeof(int) * (size_t) p->npair);
    p->pair_n = (int *) malloc(sizeof(int) * (size_t) p->npair);
    p->pair_off = (size_t *) malloc(sizeof(size_t) * ((size_t) p->npair + 1));
    p->pair_first = (int *) malloc(sizeof(int) * ((size_t) p->nranks + 1));
    p->group_first = (int *) malloc(sizeof(int) * ((size_t) p->nsh_p + 1));
    p->group_row = (int *) malloc(sizeof(int) * ((size_t) p->nsh_p + 1));
    p->ao_counts = (int *) malloc(sizeof(int) * (size_t) p->nranks);
    p->ao_displs = (int *) malloc(sizeof(int) * (size_t) p->nranks);
    p->q_counts = (int *) malloc(sizeof(int) * (size_t) p->nranks);
    p->q_displs = (int *) malloc(sizeof(int) * ((size_t) p->nranks + 1));
    if (p->pair_m == NULL || p->pair_n == NULL || p->pair_off == NULL
        || p->pair_first == NULL || p->group_first == NULL
        || p->group_row == NULL || p->ao_counts == NULL
        || p->ao_displs == NULL || p->q_counts == NULL || p->q_displs == NULL)
    {
        pdf_free(p);
        return CINT_STATUS_ALLOC_FAILED;
    }

    size_t off = 0;
    int ip = 0;
    for (int M = 0; M < p->nsh_p; M++)
    {
        for (int N = 0; N <= M; N++, ip++)
        {
            p->pair_m[ip] = M;
            p->pair_n[ip] = N;
            p->pair_off[ip] = off;
            off += (size_t) GTFDF_priShellDim(p->df, M)
                 * GTFDF_priShellDim(p->df, N);
        }
    }
    p->pair_off[p->npair] = off;
    p->npair_ao = off;

    /*
     * Greedy runs of shells up to panel_rows AO rows each. Shells are ordered
     * by AO index, so a run of them covers a contiguous band of rows.
     */
    p->ngroups = 0;
    p->group_first[0] = 0;
    p->group_row[0] = 0;
    for (int sh = 0, rows = 0; sh < p->nsh_p; sh++)
    {
        int d = GTFDF_priShellDim(p->df, sh);
        if (rows > 0 && rows + d > p->panel_rows)
        {
            p->group_first[p->ngroups + 1] = sh;
            p->group_row[++p->ngroups] = GTFDF_priFuncStart(p->df, sh);
            rows = 0;
        }
        rows += d;
    }
    p->group_first[p->ngroups + 1] = p->nsh_p;
    p->group_row[++p->ngroups] = p->nbf;

    /*
     * The MPI strided types and the BLAS leading dimensions below both index
     * the packed pair axis with a 32-bit int, and PDF_computeJK reduces
     * 2 * nbf^2 doubles in one call. Both cap this near 32k basis functions.
     * Fail loudly rather than silently wrap.
     */
    if (p->npair_ao > (size_t) INT_MAX
        || (size_t) p->nbf * p->nbf > (size_t) INT_MAX / 2)
    {
        pdf_free(p);
        return CINT_STATUS_INVALID_VALUE;
    }

    pdf_partition_pairs(p);

    double cond = fitting_cond > 0.0 ? fitting_cond : PDF_DEFAULT_COND;
    double *metric =
        (double *) malloc(sizeof(double) * (size_t) p->naux * p->naux);
    if (metric == NULL)
    {
        pdf_free(p);
        return CINT_STATUS_ALLOC_FAILED;
    }
    double t0 = MPI_Wtime();
    status = pdf_build_metric(p, metric);
    p->phase_s[PDF_PHASE_METRIC] = MPI_Wtime() - t0;

    int *piv = NULL;
    if (status == CINT_STATUS_SUCCESS)
    {
        t0 = MPI_Wtime();
        status = pdf_factor_metric(p, metric, cond, &piv);
        p->phase_s[PDF_PHASE_FACTOR] = MPI_Wtime() - t0;
    }
    if (status != CINT_STATUS_SUCCESS)
    {
        free(metric);
        free(piv);
        pdf_free(p);
        return status;
    }
    /* Only the retained rows are distributed, so this waits on the factor. */
    pdf_partition_aux(p);

    /*
     * Phases A and B share one buffer. The three-center block is built over
     * every auxiliary function, then reordered and solved against in place, so
     * the fitted rows overwrite their own input and setup never holds two
     * copies of the tensor at once.
     */
    size_t local_tensor = (size_t) p->naux * p->ao_local;
    double *A3 = NULL;
    if (local_tensor > 0)
    {
        A3 = (double *) malloc(sizeof(double) * local_tensor);
        if (A3 == NULL)
        {
            free(metric);
            free(piv);
            pdf_free(p);
            return CINT_STATUS_ALLOC_FAILED;
        }
    }

    t0 = MPI_Wtime();
    status = pdf_build_3c(p, A3);
    p->phase_s[PDF_PHASE_INT3C] = MPI_Wtime() - t0;

    t0 = MPI_Wtime();
    if (status == CINT_STATUS_SUCCESS)
        status = pdf_permute_aux_rows(p, A3, piv);
    if (status == CINT_STATUS_SUCCESS && local_tensor > 0)
    {
        /*
         * Phase B: solve B L^T = P^T A3 from the right, which in this
         * row-major buffer is X = (P^T A3)^T L^-T = B^T. Rows nfit and beyond
         * are left as the integrals put them and are never read again.
         */
        double one = 1.0;
        dtrsm_("R", "L", "T", "N", &p->ao_local, &p->nfit, &one, metric,
               &p->naux, A3, &p->ao_local);
    }
    p->phase_s[PDF_PHASE_FIT] = MPI_Wtime() - t0;
    free(metric);
    free(piv);
    if (status != CINT_STATUS_SUCCESS)
    {
        free(A3);
        pdf_free(p);
        return status;
    }

    t0 = MPI_Wtime();
    if (p->nranks == 1 && A3 != NULL)
    {
        /*
         * At one rank the fitted block already is this rank's slice: q_local is
         * nfit and npair_ao is ao_local, so the two layouts coincide. Adopt the
         * buffer rather than copying it, and hand the unused tail back. A
         * failed shrink is not an error; the oversized block is still correct.
         */
        size_t bsize = (size_t) p->nfit * p->ao_local;
        p->B = A3;
        A3 = NULL;
        double *shrunk =
            (double *) realloc(p->B, sizeof(double) * (bsize > 0 ? bsize : 1));
        if (shrunk != NULL) p->B = shrunk;
    }
    else
    {
        size_t bsize = (size_t) p->q_local * p->npair_ao;
        p->B = (double *) malloc(sizeof(double) * (bsize > 0 ? bsize : 1));
        if (p->B == NULL)
        {
            free(A3);
            pdf_free(p);
            return CINT_STATUS_ALLOC_FAILED;
        }
        status = pdf_redistribute(p, A3);
        free(A3);
    }
    p->phase_s[PDF_PHASE_REDIST] = MPI_Wtime() - t0;
    if (status != CINT_STATUS_SUCCESS)
    {
        pdf_free(p);
        return status;
    }

    size_t nbf2 = (size_t) p->nbf * p->nbf;
    p->dvec = (double *) malloc(sizeof(double) * p->npair_ao);
    p->cvec = (double *) malloc(sizeof(double) * (p->q_local > 0 ? p->q_local : 1));
    p->jkbuf = (double *) malloc(sizeof(double) * 2 * nbf2);
    p->panels = (double *) malloc(sizeof(double) * (size_t) p->nthreads
                                  * p->panel_rows * p->nbf);
    if (p->dvec == NULL || p->cvec == NULL || p->jkbuf == NULL
        || p->panels == NULL)
    {
        pdf_free(p);
        return CINT_STATUS_ALLOC_FAILED;
    }

    *pdf_out = p;
    return CINT_STATUS_SUCCESS;
}

CIntStatus_t PDF_destroy(PDF_t pdf)
{
    pdf_free(pdf);
    return CINT_STATUS_SUCCESS;
}

/* ------------------------------------------------------------ iteration -- */

/*
 * Write primary shell M's rows of one auxiliary function's B_Q into dst, dM
 * dense rows of nbf doubles each, as one shell's contribution to a band.
 *
 * Only unique shell pairs are stored, so a band reads the (M,N) block straight
 * for N <= M and the (N,M) block transposed for N > M. Every write runs along a
 * band row, which is the whole point: the dense nbf x nbf scatter this replaced
 * wrote in dM x dN patches strided by nbf, touching a cache line per few useful
 * doubles, and measured an order of magnitude under this machine's streaming
 * bandwidth. A band is a few hundred kilobytes and is read straight back by the
 * GEMM, so the densified slice never reaches memory at all.
 *
 * The copies are written out element by element rather than handed to memcpy: a
 * block row is a couple of doubles wide, so a call whose length is only known
 * at run time costs more than the copy it performs, and there are one of them
 * per (auxiliary function, shell pair, row).
 *
 * A diagonal block holds the full square and is symmetric, so the straight
 * copy covers it; unlike the scatter, nothing writes an element twice.
 */
static void pdf_shell_rows(PDF_t p, const double *row, int M, double *dst)
{
    const int nbf = p->nbf;
    const int dM = GTFDF_priShellDim(p->df, M);
    const size_t rowM = (size_t) M * (M + 1) / 2;

    for (int N = 0; N <= M; N++)
    {
        int dN = GTFDF_priShellDim(p->df, N);
        int sN = GTFDF_priFuncStart(p->df, N);
        const double *blk = &row[p->pair_off[rowM + N]];
        for (int m = 0; m < dM; m++)
        {
            double *out = &dst[(size_t) m * nbf + sN];
            const double *src = &blk[m * dN];
            for (int n = 0; n < dN; n++) out[n] = src[n];
        }
    }
    for (int N = M + 1; N < p->nsh_p; N++)
    {
        int dN = GTFDF_priShellDim(p->df, N);
        int sN = GTFDF_priFuncStart(p->df, N);
        const double *blk = &row[p->pair_off[(size_t) N * (N + 1) / 2 + M]];
        for (int m = 0; m < dM; m++)
        {
            double *out = &dst[(size_t) m * nbf + sN];
            for (int n = 0; n < dN; n++) out[n] = blk[n * dM + m];
        }
    }
}

CIntStatus_t PDF_computeJK(PDF_t pdf, const double *D, const double *Cocc,
                           int nocc, double *J, double *K)
{
    if (pdf == NULL) return CINT_STATUS_INVALID_VALUE;
    if (J != NULL && D == NULL) return CINT_STATUS_INVALID_VALUE;
    if (K != NULL && (Cocc == NULL || nocc < 0)) return CINT_STATUS_INVALID_VALUE;

    const int nbf = pdf->nbf;
    const size_t nbf2 = (size_t) nbf * nbf;
    const int npair_ao = (int) pdf->npair_ao;
    double *Jloc = pdf->jkbuf;
    double *Kloc = pdf->jkbuf + nbf2;

    /*
     * Size the batch and grow the half-transform scratch before any compute:
     * bailing out from inside the loops below would strand the other ranks in
     * the Allreduce.
     */
    int failed = 0;
    int q_batch = 1;
    if (K != NULL && nocc > 0 && pdf->q_local > 0)
    {
        size_t per_q = (size_t) nbf * nocc;
        size_t budget = PDF_K_BATCH_BYTES / (sizeof(double) * per_q);
        q_batch = (budget > PDF_K_MAX_BATCH) ? PDF_K_MAX_BATCH : (int) budget;
        if (q_batch < 1) q_batch = 1;
        if (q_batch > pdf->q_local) q_batch = pdf->q_local;
        size_t need = per_q * (size_t) q_batch;
        if (need > pdf->half_cap)
        {
            free(pdf->half);
            pdf->half = (double *) malloc(sizeof(double) * need);
            pdf->half_cap = (pdf->half != NULL) ? need : 0;
            failed = (pdf->half == NULL);
        }
    }

    memset(pdf->jkbuf, 0, sizeof(double) * 2 * nbf2);

    double t0 = MPI_Wtime();

    if (J != NULL && pdf->q_local > 0)
    {
        /*
         * Pack D onto the packed pair axis. An off-diagonal shell-pair block
         * stands in for both (M,N) and (N,M), so it carries weight two; a
         * diagonal block stores the full square and carries weight one.
         */
#pragma omp parallel for schedule(dynamic) num_threads(pdf->nthreads)
        for (int ip = 0; ip < pdf->npair; ip++)
        {
            int M = pdf->pair_m[ip];
            int N = pdf->pair_n[ip];
            int dM = GTFDF_priShellDim(pdf->df, M);
            int dN = GTFDF_priShellDim(pdf->df, N);
            int sM = GTFDF_priFuncStart(pdf->df, M);
            int sN = GTFDF_priFuncStart(pdf->df, N);
            double w = (M == N) ? 1.0 : 2.0;
            double *dst = &pdf->dvec[pdf->pair_off[ip]];
            for (int m = 0; m < dM; m++)
                for (int n = 0; n < dN; n++)
                    dst[m * dN + n] =
                        w * D[(size_t) (sM + m) * nbf + (sN + n)];
        }

        /*
         * c_Q = sum_mn B_Q^mn D_mn, then the packed J = sum_Q B_Q c_Q. This
         * rank owns every mn for its own Q range, so c needs no reduction;
         * only the assembled J does.
         */
        double one = 1.0, zero = 0.0;
        int inc = 1;
        dgemv_("T", &npair_ao, &pdf->q_local, &one, pdf->B, &npair_ao,
               pdf->dvec, &inc, &zero, pdf->cvec, &inc);
        dgemv_("N", &npair_ao, &pdf->q_local, &one, pdf->B, &npair_ao,
               pdf->cvec, &inc, &zero, pdf->dvec, &inc);

#pragma omp parallel for schedule(dynamic) num_threads(pdf->nthreads)
        for (int ip = 0; ip < pdf->npair; ip++)
        {
            int M = pdf->pair_m[ip];
            int N = pdf->pair_n[ip];
            int dM = GTFDF_priShellDim(pdf->df, M);
            int dN = GTFDF_priShellDim(pdf->df, N);
            int sM = GTFDF_priFuncStart(pdf->df, M);
            int sN = GTFDF_priFuncStart(pdf->df, N);
            const double *src = &pdf->dvec[pdf->pair_off[ip]];
            for (int m = 0; m < dM; m++)
            {
                for (int n = 0; n < dN; n++)
                {
                    double v = src[m * dN + n];
                    Jloc[(size_t) (sM + m) * nbf + (sN + n)] = v;
                    Jloc[(size_t) (sN + n) * nbf + (sM + m)] = v;
                }
            }
        }
    }

    if (K != NULL && nocc > 0 && pdf->q_local > 0 && !failed)
    {
        /*
         * Half-transform straight off the packed blocks. One (auxiliary
         * function, shell group) pair gathers that group's band of B_Q rows
         * into a cache-resident panel and multiplies it by the occupied
         * coefficients; the result lands in its own corner of the batch
         * buffer, so no two iterations of the loop write the same output and
         * the buffer needs no zeroing.
         *
         * The dgemm runs inside the parallel region. OpenMP nesting is off by
         * default, so a threaded BLAS serialises there, which is what we
         * want: the loop over auxiliary functions and bands already has every
         * thread busy, and running the GEMM there is what keeps each band on
         * the core that built it, in that core's own cache.
         *
         * A whole batch then shares one dsyrk. That is the same K += H H^T as
         * one auxiliary function at a time, but it passes over the nbf x nbf
         * accumulator once per batch rather than once per Q.
         */
        const int ngroups = pdf->ngroups;
        double one = 1.0, zero = 0.0;
        for (int qb = 0; qb < pdf->q_local; qb += q_batch)
        {
            int nq = pdf->q_local - qb;
            if (nq > q_batch) nq = q_batch;
            const int ldh = nq * nocc;
#pragma omp parallel num_threads(pdf->nthreads)
            {
                double *panel = &pdf->panels[(size_t) omp_get_thread_num()
                                             * pdf->panel_rows * nbf];
#pragma omp for collapse(2) schedule(dynamic)
                for (int qi = 0; qi < nq; qi++)
                {
                    for (int g = 0; g < ngroups; g++)
                    {
                        const double *row =
                            &pdf->B[(size_t) (qb + qi) * npair_ao];
                        int gs = pdf->group_row[g];
                        int gr = pdf->group_row[g + 1] - gs;
                        for (int M = pdf->group_first[g];
                             M < pdf->group_first[g + 1]; M++)
                        {
                            int sM = GTFDF_priFuncStart(pdf->df, M);
                            pdf_shell_rows(pdf, row, M,
                                           &panel[(size_t) (sM - gs) * nbf]);
                        }
                        /* half rows gs.. of this Q = panel * Cocc, row-major,
                         * via the Fortran transpose view. */
                        dgemm_("N", "N", &nocc, &gr, &nbf, &one, Cocc, &nocc,
                               panel, &nbf, &zero,
                               &pdf->half[(size_t) gs * ldh + qi * nocc],
                               &ldh);
                    }
                }
            }
            /* K += half half^T, accumulating the Fortran upper triangle. */
            dsyrk_("U", "T", &nbf, &ldh, &one, pdf->half, &ldh, &one, Kloc,
                   &nbf);
        }
    }

    pdf->jk_phase_s[PDF_JK_LOCAL] += MPI_Wtime() - t0;

    t0 = MPI_Wtime();
    if (MPI_Barrier(pdf->comm) != MPI_SUCCESS)
        return CINT_STATUS_EXECUTION_FAILED;
    pdf->jk_phase_s[PDF_JK_SKEW] += MPI_Wtime() - t0;

    t0 = MPI_Wtime();
    if (MPI_Allreduce(MPI_IN_PLACE, pdf->jkbuf, (int) (2 * nbf2), MPI_DOUBLE,
                      MPI_SUM, pdf->comm) != MPI_SUCCESS)
        return CINT_STATUS_EXECUTION_FAILED;
    pdf->jk_phase_s[PDF_JK_COMM] += MPI_Wtime() - t0;
    pdf->jk_calls++;
    if (failed) return CINT_STATUS_ALLOC_FAILED;

    if (J != NULL) memcpy(J, Jloc, sizeof(double) * nbf2);
    if (K != NULL)
    {
        /* dsyrk touched only one triangle; mirror it into a full matrix. */
        for (int i = 0; i < nbf; i++)
            for (int j = 0; j < i; j++)
                Kloc[(size_t) j * nbf + i] = Kloc[(size_t) i * nbf + j];
        memcpy(K, Kloc, sizeof(double) * nbf2);
    }
    return CINT_STATUS_SUCCESS;
}

int PDF_nBasisFuncs(PDF_t pdf) { return pdf->nbf; }
int PDF_nAuxFuncs(PDF_t pdf) { return pdf->naux; }
int PDF_nFitFuncs(PDF_t pdf) { return pdf->nfit; }
int PDF_nLocalAuxFuncs(PDF_t pdf) { return pdf->q_local; }
int PDF_nMetricNullVectors(PDF_t pdf) { return pdf->nnull; }
size_t PDF_localTensorSize(PDF_t pdf)
{
    return (size_t) pdf->q_local * pdf->npair_ao;
}
int PDF_nLocalPairElements(PDF_t pdf) { return pdf->ao_local; }

double PDF_phaseSeconds(PDF_t pdf, PDF_Phase phase)
{
    if (pdf == NULL || phase < 0 || phase >= PDF_NPHASES) return 0.0;
    return pdf->phase_s[phase];
}

double PDF_jkPhaseSeconds(PDF_t pdf, PDF_JKPhase phase)
{
    if (pdf == NULL || phase < 0 || phase >= PDF_NJKPHASES) return 0.0;
    return pdf->jk_phase_s[phase];
}

int PDF_jkCalls(PDF_t pdf) { return (pdf == NULL) ? 0 : pdf->jk_calls; }

const char *PDF_jkPhaseName(PDF_JKPhase phase)
{
    switch (phase)
    {
        case PDF_JK_LOCAL: return "jk_local";
        case PDF_JK_SKEW:  return "jk_skew";
        case PDF_JK_COMM:  return "jk_comm";
        default:           return NULL;
    }
}

const char *PDF_phaseName(PDF_Phase phase)
{
    switch (phase)
    {
        case PDF_PHASE_METRIC: return "metric";
        case PDF_PHASE_FACTOR: return "factor";
        case PDF_PHASE_INT3C:  return "int3c";
        case PDF_PHASE_FIT:    return "fit";
        case PDF_PHASE_REDIST: return "redist";
        default:               return NULL;
    }
}
