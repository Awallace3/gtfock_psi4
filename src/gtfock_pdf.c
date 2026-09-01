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
extern void dsyev_(const char *jobz, const char *uplo, const int *n, double *a,
                   const int *lda, double *w, double *work, const int *lwork,
                   int *info);

#define PDF_DEFAULT_COND 1e-12

struct PDF
{
    MPI_Comm comm;
    int nranks, rank, nthreads;

    GTFDF_t df;
    int nbf, naux, nsh_p, nsh_a;
    int nnull;

    /* Unique primary shell pairs (M >= N), ordered by M then N. */
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

    /* Post-Phase-C ownership: a contiguous run of auxiliary functions. */
    int *q_counts;     /* nranks */
    int *q_displs;     /* nranks + 1 */
    int q_local;

    /* q_local x npair_ao, row-major: the fitted tensor this rank owns. */
    double *B;

    /* Per-iteration scratch, allocated once. */
    double *dvec;      /* npair_ao */
    double *cvec;      /* q_local */
    double *jkbuf;     /* 2 * nbf * nbf: J then K */
    double *slice;     /* nbf * nbf: one auxiliary function, densified */
    double *half;      /* nbf * nocc_cap */
    int nocc_cap;
};

/* ---------------------------------------------------------------- setup -- */

static void pdf_free(PDF_t p)
{
    if (p == NULL) return;
    free(p->pair_m);
    free(p->pair_n);
    free(p->pair_off);
    free(p->pair_first);
    free(p->ao_counts);
    free(p->ao_displs);
    free(p->q_counts);
    free(p->q_displs);
    free(p->B);
    free(p->dvec);
    free(p->cvec);
    free(p->jkbuf);
    free(p->slice);
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

static void pdf_partition_aux(PDF_t p)
{
    int base = p->naux / p->nranks;
    int rem  = p->naux % p->nranks;
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
 * bits and the eigendecomposition below needs no further agreement.
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
 * metric -> J^-1/2, in a freshly allocated buffer. The decomposition is
 * replicated: at naux = 7000 that is a 392 MB matrix and an O(naux^3) solve on
 * every rank, which docs/distributed-df.md flags as the first thing ScaLAPACK
 * should take over.
 */
static CIntStatus_t pdf_invsqrt_metric(PDF_t p, double *metric, double cond,
                                       double **jm12_out)
{
    const int n = p->naux;
    double *w = (double *) malloc(sizeof(double) * n);
    double *jm12 = (double *) malloc(sizeof(double) * (size_t) n * n);
    if (w == NULL || jm12 == NULL)
    {
        free(w);
        free(jm12);
        return CINT_STATUS_ALLOC_FAILED;
    }

    int info = 0, lwork = -1;
    double query = 0.0;
    dsyev_("V", "U", &n, metric, &n, w, &query, &lwork, &info);
    if (info != 0)
    {
        free(w);
        free(jm12);
        return CINT_STATUS_EXECUTION_FAILED;
    }
    lwork = (int) query;
    double *work = (double *) malloc(sizeof(double) * (size_t) lwork);
    if (work == NULL)
    {
        free(w);
        free(jm12);
        return CINT_STATUS_ALLOC_FAILED;
    }
    dsyev_("V", "U", &n, metric, &n, w, work, &lwork, &info);
    free(work);
    if (info != 0)
    {
        free(w);
        free(jm12);
        return CINT_STATUS_EXECUTION_FAILED;
    }

    /*
     * dsyev returns eigenvectors as columns in Fortran order, so in this
     * row-major buffer eigenvector k is row k. Scaling that row by
     * w[k]^-1/4 turns the outer product below into V diag(w^-1/2) V^T.
     */
    double wmax = w[n - 1] > 0.0 ? w[n - 1] : 0.0;
    double floor_ = cond * wmax;
    int nnull = 0;
    for (int k = 0; k < n; k++)
    {
        double scale = 0.0;
        if (w[k] > floor_ && w[k] > 0.0) scale = 1.0 / sqrt(sqrt(w[k]));
        else nnull++;
        double *row = &metric[(size_t) k * n];
        for (int i = 0; i < n; i++) row[i] *= scale;
    }
    p->nnull = nnull;
    free(w);

    /*
     * Read row-major metric[k][i] as the Fortran matrix A(i,k); then
     * A A^T = sum_k V_k V_k^T / sqrt(w_k), which is what we want.
     */
    double one = 1.0, zero = 0.0;
    dsyrk_("U", "N", &n, &n, &one, metric, &n, &zero, jm12, &n);
    /* dsyrk filled the Fortran upper triangle; mirror to a full matrix. */
    for (int j = 0; j < n; j++)
        for (int i = 0; i < j; i++)
            jm12[(size_t) j + (size_t) i * n] = jm12[(size_t) i + (size_t) j * n];

    *jm12_out = jm12;
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
 * Phase C: go from "all Q, my mn" to "my Q, all mn".
 *
 * MPI_Alltoallw would express this in one call but its displacements are byte
 * counts in int, which overflow well below the 24 GB/rank this is sized for.
 * Phased pairwise Sendrecv with a strided receive type has the same message
 * count, needs no third copy, and has no such ceiling.
 */
static CIntStatus_t pdf_redistribute(PDF_t p, const double *Bfull)
{
    const int npair_ao = (int) p->npair_ao;
    const int ao_local = p->ao_local;

    for (int q = 0; q < p->q_local; q++)
        memcpy(&p->B[(size_t) q * npair_ao + p->ao_displs[p->rank]],
               &Bfull[(size_t) (p->q_displs[p->rank] + q) * ao_local],
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
            ? &Bfull[(size_t) p->q_displs[dst] * ao_local] : p->B;

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

    p->pair_m = (int *) malloc(sizeof(int) * (size_t) p->npair);
    p->pair_n = (int *) malloc(sizeof(int) * (size_t) p->npair);
    p->pair_off = (size_t *) malloc(sizeof(size_t) * ((size_t) p->npair + 1));
    p->pair_first = (int *) malloc(sizeof(int) * ((size_t) p->nranks + 1));
    p->ao_counts = (int *) malloc(sizeof(int) * (size_t) p->nranks);
    p->ao_displs = (int *) malloc(sizeof(int) * (size_t) p->nranks);
    p->q_counts = (int *) malloc(sizeof(int) * (size_t) p->nranks);
    p->q_displs = (int *) malloc(sizeof(int) * ((size_t) p->nranks + 1));
    if (p->pair_m == NULL || p->pair_n == NULL || p->pair_off == NULL
        || p->pair_first == NULL || p->ao_counts == NULL || p->ao_displs == NULL
        || p->q_counts == NULL || p->q_displs == NULL)
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
    pdf_partition_aux(p);

    double cond = fitting_cond > 0.0 ? fitting_cond : PDF_DEFAULT_COND;
    double *metric =
        (double *) malloc(sizeof(double) * (size_t) p->naux * p->naux);
    if (metric == NULL)
    {
        pdf_free(p);
        return CINT_STATUS_ALLOC_FAILED;
    }
    status = pdf_build_metric(p, metric);
    double *jm12 = NULL;
    if (status == CINT_STATUS_SUCCESS)
        status = pdf_invsqrt_metric(p, metric, cond, &jm12);
    free(metric);
    if (status != CINT_STATUS_SUCCESS)
    {
        free(jm12);
        pdf_free(p);
        return status;
    }

    size_t local_tensor = (size_t) p->naux * p->ao_local;
    double *A3 = NULL, *Bfull = NULL;
    if (local_tensor > 0)
    {
        A3 = (double *) malloc(sizeof(double) * local_tensor);
        Bfull = (double *) malloc(sizeof(double) * local_tensor);
        if (A3 == NULL || Bfull == NULL)
        {
            free(A3);
            free(Bfull);
            free(jm12);
            pdf_free(p);
            return CINT_STATUS_ALLOC_FAILED;
        }
    }

    status = pdf_build_3c(p, A3);
    if (status == CINT_STATUS_SUCCESS && local_tensor > 0)
    {
        /* Phase B: Bfull = J^-1/2 A3, read column-major as Bfull^T = A3^T J^-1/2. */
        double one = 1.0, zero = 0.0;
        dgemm_("N", "N", &p->ao_local, &p->naux, &p->naux, &one, A3,
               &p->ao_local, jm12, &p->naux, &zero, Bfull, &p->ao_local);
    }
    free(A3);
    free(jm12);
    if (status != CINT_STATUS_SUCCESS)
    {
        free(Bfull);
        pdf_free(p);
        return status;
    }

    size_t bsize = (size_t) p->q_local * p->npair_ao;
    p->B = (double *) malloc(sizeof(double) * (bsize > 0 ? bsize : 1));
    if (p->B == NULL)
    {
        free(Bfull);
        pdf_free(p);
        return CINT_STATUS_ALLOC_FAILED;
    }
    status = pdf_redistribute(p, Bfull);
    free(Bfull);
    if (status != CINT_STATUS_SUCCESS)
    {
        pdf_free(p);
        return status;
    }

    size_t nbf2 = (size_t) p->nbf * p->nbf;
    p->dvec = (double *) malloc(sizeof(double) * p->npair_ao);
    p->cvec = (double *) malloc(sizeof(double) * (p->q_local > 0 ? p->q_local : 1));
    p->jkbuf = (double *) malloc(sizeof(double) * 2 * nbf2);
    p->slice = (double *) malloc(sizeof(double) * nbf2);
    if (p->dvec == NULL || p->cvec == NULL || p->jkbuf == NULL
        || p->slice == NULL)
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

/* Scatter one auxiliary function's packed pair row into a dense nbf x nbf. */
static void pdf_densify(PDF_t p, const double *row, double *dense)
{
    const int nbf = p->nbf;
#pragma omp parallel for schedule(dynamic) num_threads(p->nthreads)
    for (int ip = 0; ip < p->npair; ip++)
    {
        int M = p->pair_m[ip];
        int N = p->pair_n[ip];
        int dM = GTFDF_priShellDim(p->df, M);
        int dN = GTFDF_priShellDim(p->df, N);
        int sM = GTFDF_priFuncStart(p->df, M);
        int sN = GTFDF_priFuncStart(p->df, N);
        const double *blk = &row[p->pair_off[ip]];
        for (int m = 0; m < dM; m++)
        {
            for (int n = 0; n < dN; n++)
            {
                double v = blk[m * dN + n];
                dense[(size_t) (sM + m) * nbf + (sN + n)] = v;
                dense[(size_t) (sN + n) * nbf + (sM + m)] = v;
            }
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
     * Grow the half-transform scratch before any compute: bailing out from
     * inside the loops below would strand the other ranks in the Allreduce.
     */
    int failed = 0;
    if (K != NULL && nocc > 0 && nocc > pdf->nocc_cap)
    {
        free(pdf->half);
        pdf->half = (double *) malloc(sizeof(double) * (size_t) nbf * nocc);
        pdf->nocc_cap = (pdf->half != NULL) ? nocc : 0;
        failed = (pdf->half == NULL);
    }

    memset(pdf->jkbuf, 0, sizeof(double) * 2 * nbf2);

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
         * One auxiliary function at a time: densify B_Q, half-transform it
         * with the occupied coefficients, and accumulate the outer product.
         * Batching several Q into one GEMM would amortise the call overhead;
         * at SCF-relevant sizes each of these is already a large GEMM.
         */
        double one = 1.0, zero = 0.0;
        for (int q = 0; q < pdf->q_local; q++)
        {
            pdf_densify(pdf, &pdf->B[(size_t) q * npair_ao], pdf->slice);
            /* half = slice * Cocc, row-major, via the Fortran transpose view. */
            dgemm_("N", "N", &nocc, &nbf, &nbf, &one, Cocc, &nocc, pdf->slice,
                   &nbf, &zero, pdf->half, &nocc);
            /* K += half half^T, accumulating the Fortran upper triangle. */
            dsyrk_("U", "T", &nbf, &nocc, &one, pdf->half, &nocc, &one, Kloc,
                   &nbf);
        }
    }

    if (MPI_Allreduce(MPI_IN_PLACE, pdf->jkbuf, (int) (2 * nbf2), MPI_DOUBLE,
                      MPI_SUM, pdf->comm) != MPI_SUCCESS)
        return CINT_STATUS_EXECUTION_FAILED;
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
int PDF_nLocalAuxFuncs(PDF_t pdf) { return pdf->q_local; }
int PDF_nMetricNullVectors(PDF_t pdf) { return pdf->nnull; }
size_t PDF_localTensorSize(PDF_t pdf)
{
    return (size_t) pdf->q_local * pdf->npair_ao;
}
int PDF_nLocalPairElements(PDF_t pdf) { return pdf->ao_local; }
