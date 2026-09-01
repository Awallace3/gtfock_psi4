/*
 * Three- and two-center Coulomb integrals for distributed density fitting.
 * See docs/distributed-df.md for the design this serves.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <simint/simint.h>

#include <CInt.h>
#include <cint_def.h>

#include "gtfock_df.h"

/* libcint keeps these file-local in cint_basisset.c; only the value is API. */
#define GTFDF_BASISTYPE_CARTESIAN 0

#define GTFDF_NCART(am) (((am) + 1) * ((am) + 2) / 2)

struct GTFDF
{
    int nthreads;
    int max_am;
    int workmem_per_thread;
    int outmem_per_thread;
    double *workbuf;
    double *outbuf;

    int nsh_p, nbf_p;
    int nsh_a, nbf_a;
    int *pri_dim, *pri_start;
    int *aux_dim, *aux_start;

    struct simint_shell *pri_shells;
    struct simint_shell *aux_shells;
    struct simint_shell zero_shell;

    /* (M,N) over the primary basis, row-major in nsh_p. */
    struct simint_multi_shellpair *mn_pairs;
    /* (P,zero) over the auxiliary basis; serves both 3- and 2-center calls. */
    struct simint_multi_shellpair *aux_pairs;

    int screen_method;
    double screen_tol;
};

static void *gtfdf_alloc64(size_t bytes)
{
    void *p = NULL;
    size_t padded = (bytes + 63) / 64 * 64;
    if (posix_memalign(&p, 64, padded) != 0) return NULL;
    return p;
}

/*
 * Mirrors CInt_createSIMINT's shell construction. The Cartesian import path in
 * libcint leaves bs_cc untouched (its normalization() returns early for
 * CARTESIAN), so simint_normalize_shells supplies the whole normalization,
 * exactly as it does for the exact four-center path.
 */
static struct simint_shell *gtfdf_build_shells(BasisSet_t basis)
{
    int nshells = (int) basis->nshells;
    struct simint_shell *shells =
        (struct simint_shell *) malloc(sizeof(struct simint_shell) * nshells);
    if (shells == NULL) return NULL;

    for (int i = 0; i < nshells; i++)
    {
        struct simint_shell *sh = &shells[i];
        simint_initialize_shell(sh);
        simint_allocate_shell((int) basis->nexp[i], sh);

        sh->am    = (int) basis->momentum[i];
        sh->nprim = (int) basis->nexp[i];
        sh->x     = basis->xyz0[i * 4 + 0];
        sh->y     = basis->xyz0[i * 4 + 1];
        sh->z     = basis->xyz0[i * 4 + 2];

        for (int j = 0; j < (int) basis->nexp[i]; j++)
        {
            sh->alpha[j] = basis->exp[i][j];
            sh->coef[j]  = basis->cc[i][j];
        }
    }

    simint_normalize_shells(nshells, shells);
    return shells;
}

static int gtfdf_fill_offsets(BasisSet_t basis, int **dim_out, int **start_out)
{
    int nshells = (int) basis->nshells;
    int *dim   = (int *) malloc(sizeof(int) * nshells);
    int *start = (int *) malloc(sizeof(int) * nshells);
    if (dim == NULL || start == NULL)
    {
        free(dim);
        free(start);
        return -1;
    }
    for (int i = 0; i < nshells; i++)
    {
        start[i] = (int) basis->f_start_id[i];
        dim[i]   = (int) basis->f_end_id[i] - start[i] + 1;
    }
    *dim_out   = dim;
    *start_out = start;
    return 0;
}

CIntStatus_t GTFDF_create(BasisSet_t primary, BasisSet_t auxiliary,
                          int nthreads, GTFDF_t *df)
{
    if (primary == NULL || auxiliary == NULL || df == NULL || nthreads <= 0)
        return CINT_STATUS_INVALID_VALUE;
    if (primary->basistype != GTFDF_BASISTYPE_CARTESIAN ||
        auxiliary->basistype != GTFDF_BASISTYPE_CARTESIAN)
    {
        fprintf(stderr, "GTFDF_create: both basis sets must be Cartesian\n");
        return CINT_STATUS_INVALID_VALUE;
    }

    struct GTFDF *s = (struct GTFDF *) calloc(1, sizeof(struct GTFDF));
    if (s == NULL) return CINT_STATUS_ALLOC_FAILED;

    /* Idempotent in effect for our use: CInt_createSIMINT calls this too, and
     * neither owner may call simint_finalize() while the other is alive. */
    simint_init();

    s->nthreads = nthreads;
    s->nsh_p = (int) primary->nshells;
    s->nbf_p = (int) primary->nfunctions;
    s->nsh_a = (int) auxiliary->nshells;
    s->nbf_a = (int) auxiliary->nfunctions;

    s->max_am = (int) primary->max_momentum;
    if ((int) auxiliary->max_momentum > s->max_am)
        s->max_am = (int) auxiliary->max_momentum;
    if (s->max_am > SIMINT_OSTEI_MAXAM)
    {
        fprintf(stderr,
                "GTFDF_create: angular momentum %d exceeds the maximum of %d "
                "this Simint was generated for\n",
                s->max_am, SIMINT_OSTEI_MAXAM);
        GTFDF_destroy(s);
        return CINT_STATUS_INVALID_VALUE;
    }

    /* Sized from the observed angular momenta, never from libcint's
     * _SIMINT_OSTEI_MAXAM, which is stale against the generated Simint. */
    s->workmem_per_thread = simint_ostei_workmem(0, s->max_am);
    s->workmem_per_thread = (s->workmem_per_thread + 7) / 8 * 8;

    int max_ncart = GTFDF_NCART(s->max_am);
    /* Largest block is 3-center: ncart(P) * 1 * ncart(M) * ncart(N). */
    int maxsize = max_ncart * max_ncart * max_ncart;
    maxsize = (maxsize + 7) / 8 * 8;
    /* +8 for the primitive-screening statistics Simint appends. */
    s->outmem_per_thread = maxsize * SIMINT_NSHELL_SIMD + 8;

    s->workbuf = (double *) gtfdf_alloc64(
        (size_t) s->workmem_per_thread * nthreads * sizeof(double));
    s->outbuf = (double *) gtfdf_alloc64(
        (size_t) s->outmem_per_thread * nthreads * sizeof(double));
    if (s->workbuf == NULL || s->outbuf == NULL)
    {
        GTFDF_destroy(s);
        return CINT_STATUS_ALLOC_FAILED;
    }

    s->pri_shells = gtfdf_build_shells(primary);
    s->aux_shells = gtfdf_build_shells(auxiliary);
    if (s->pri_shells == NULL || s->aux_shells == NULL ||
        gtfdf_fill_offsets(primary, &s->pri_dim, &s->pri_start) != 0 ||
        gtfdf_fill_offsets(auxiliary, &s->aux_dim, &s->aux_start) != 0)
    {
        GTFDF_destroy(s);
        return CINT_STATUS_ALLOC_FAILED;
    }

    /* Built after simint_normalize_shells and deliberately excluded from it:
     * normalizing a zero-exponent shell divides by zero. */
    simint_initialize_shell(&s->zero_shell);
    simint_create_zero_shell(&s->zero_shell);

    s->screen_method = SIMINT_SCREEN_FASTSCHWARZ;
    s->screen_tol    = 1.e-14;

    /* calloc, not malloc: a later allocation failure sends these arrays
     * straight to GTFDF_destroy, which needs every ptr field already NULL. */
    s->mn_pairs = (struct simint_multi_shellpair *) calloc(
        (size_t) s->nsh_p * s->nsh_p, sizeof(struct simint_multi_shellpair));
    s->aux_pairs = (struct simint_multi_shellpair *) calloc(
        (size_t) s->nsh_a, sizeof(struct simint_multi_shellpair));
    if (s->mn_pairs == NULL || s->aux_pairs == NULL)
    {
        GTFDF_destroy(s);
        return CINT_STATUS_ALLOC_FAILED;
    }

    for (int i = 0; i < s->nsh_p; i++)
    {
        for (int j = 0; j < s->nsh_p; j++)
        {
            struct simint_multi_shellpair *pair = &s->mn_pairs[i * s->nsh_p + j];
            simint_initialize_multi_shellpair(pair);
            simint_create_multi_shellpair(1, s->pri_shells + i,
                                          1, s->pri_shells + j,
                                          pair, s->screen_method);
        }
    }
    for (int i = 0; i < s->nsh_a; i++)
    {
        struct simint_multi_shellpair *pair = &s->aux_pairs[i];
        simint_initialize_multi_shellpair(pair);
        simint_create_multi_shellpair(1, s->aux_shells + i,
                                      1, &s->zero_shell,
                                      pair, s->screen_method);
    }

    *df = s;
    return CINT_STATUS_SUCCESS;
}

CIntStatus_t GTFDF_destroy(GTFDF_t df)
{
    if (df == NULL) return CINT_STATUS_SUCCESS;

    if (df->mn_pairs != NULL)
    {
        for (size_t i = 0; i < (size_t) df->nsh_p * df->nsh_p; i++)
            simint_free_multi_shellpair(&df->mn_pairs[i]);
        free(df->mn_pairs);
    }
    if (df->aux_pairs != NULL)
    {
        for (int i = 0; i < df->nsh_a; i++)
            simint_free_multi_shellpair(&df->aux_pairs[i]);
        free(df->aux_pairs);
    }
    if (df->pri_shells != NULL)
    {
        for (int i = 0; i < df->nsh_p; i++) simint_free_shell(&df->pri_shells[i]);
        free(df->pri_shells);
    }
    if (df->aux_shells != NULL)
    {
        for (int i = 0; i < df->nsh_a; i++) simint_free_shell(&df->aux_shells[i]);
        free(df->aux_shells);
    }
    simint_free_shell(&df->zero_shell);

    free(df->pri_dim);
    free(df->pri_start);
    free(df->aux_dim);
    free(df->aux_start);
    free(df->workbuf);
    free(df->outbuf);
    free(df);
    /* No simint_finalize(): a CInt SIMINT_t may share the same global tables. */
    return CINT_STATUS_SUCCESS;
}

static CIntStatus_t gtfdf_compute(GTFDF_t df, int tid,
                                  struct simint_multi_shellpair *bra,
                                  struct simint_multi_shellpair *ket,
                                  int size, double **ints, int *nints)
{
    int ret = simint_compute_eri(bra, ket, df->screen_tol,
                                 &df->workbuf[(size_t) tid * df->workmem_per_thread],
                                 &df->outbuf[(size_t) tid * df->outmem_per_thread]);

    *ints = &df->outbuf[(size_t) tid * df->outmem_per_thread];
    /* ret < 0 means fully screened: the output buffer was never written. */
    *nints = (ret < 0) ? 0 : size;
    return CINT_STATUS_SUCCESS;
}

CIntStatus_t GTFDF_compute3c(GTFDF_t df, int tid, int P, int M, int N,
                             double **ints, int *nints)
{
    if (df == NULL || ints == NULL || nints == NULL)
        return CINT_STATUS_INVALID_VALUE;
    if (tid < 0 || tid >= df->nthreads ||
        P < 0 || P >= df->nsh_a || M < 0 || M >= df->nsh_p ||
        N < 0 || N >= df->nsh_p)
        return CINT_STATUS_INVALID_VALUE;

    int size = df->aux_dim[P] * df->pri_dim[M] * df->pri_dim[N];
    return gtfdf_compute(df, tid, &df->aux_pairs[P],
                         &df->mn_pairs[M * df->nsh_p + N], size, ints, nints);
}

CIntStatus_t GTFDF_compute2c(GTFDF_t df, int tid, int P, int Q,
                             double **ints, int *nints)
{
    if (df == NULL || ints == NULL || nints == NULL)
        return CINT_STATUS_INVALID_VALUE;
    if (tid < 0 || tid >= df->nthreads ||
        P < 0 || P >= df->nsh_a || Q < 0 || Q >= df->nsh_a)
        return CINT_STATUS_INVALID_VALUE;

    int size = df->aux_dim[P] * df->aux_dim[Q];
    return gtfdf_compute(df, tid, &df->aux_pairs[P], &df->aux_pairs[Q],
                         size, ints, nints);
}

int GTFDF_nPriShells(GTFDF_t df) { return df->nsh_p; }
int GTFDF_nPriFuncs(GTFDF_t df) { return df->nbf_p; }
int GTFDF_priShellDim(GTFDF_t df, int shell) { return df->pri_dim[shell]; }
int GTFDF_priFuncStart(GTFDF_t df, int shell) { return df->pri_start[shell]; }

int GTFDF_maxSupportedAM(void) { return SIMINT_OSTEI_MAXAM; }

int GTFDF_nAuxShells(GTFDF_t df) { return df->nsh_a; }
int GTFDF_nAuxFuncs(GTFDF_t df) { return df->nbf_a; }
int GTFDF_auxShellDim(GTFDF_t df, int shell) { return df->aux_dim[shell]; }
int GTFDF_auxFuncStart(GTFDF_t df, int shell) { return df->aux_start[shell]; }

double GTFDF_getScreenTol(GTFDF_t df) { return df->screen_tol; }

CIntStatus_t GTFDF_setScreenTol(GTFDF_t df, double tol)
{
    if (df == NULL || tol < 0.0) return CINT_STATUS_INVALID_VALUE;
    df->screen_tol = tol;
    return CINT_STATUS_SUCCESS;
}
