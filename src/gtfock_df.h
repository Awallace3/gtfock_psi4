/*
 * Three- and two-center Coulomb integrals for distributed density fitting.
 *
 * libcint offers four-center quartets over a single BasisSet_t only. This
 * engine drives Simint directly over a primary/auxiliary basis pair, using the
 * zero shell Simint provides for exactly this purpose. See
 * docs/distributed-df.md.
 */

#ifndef GTFOCK_DF_H
#define GTFOCK_DF_H

/* CInt.h uses NULL without including <stddef.h>; pull it in first so this
 * header stays self-contained for consumers. */
#include <stddef.h>

#include <CInt.h>

#ifdef __cplusplus
extern "C" {
#endif

struct GTFDF;
typedef struct GTFDF *GTFDF_t;

/*
 * Both basis sets must be Cartesian (imported with pure == 0), as the exact
 * GTFock path also requires. `primary` and `auxiliary` are borrowed, not owned,
 * and must outlive the returned handle.
 */
CIntStatus_t GTFDF_create(BasisSet_t primary, BasisSet_t auxiliary,
                          int nthreads, GTFDF_t *df);

CIntStatus_t GTFDF_destroy(GTFDF_t df);

/*
 * (P|MN) over Cartesian components, laid out p * (dM * dN) + m * dN + n, where
 * dM and dN are GTFDF_priShellDim() of M and N.
 *
 * `*ints` points into per-thread storage owned by `df` and is valid until the
 * next call on the same `tid`. A fully screened quartet returns
 * CINT_STATUS_SUCCESS with `*nints == 0` and an uninitialized buffer.
 */
CIntStatus_t GTFDF_compute3c(GTFDF_t df, int tid, int P, int M, int N,
                             double **ints, int *nints);

/* (P|Q) over Cartesian components, laid out p * dQ + q. Same buffer rules. */
CIntStatus_t GTFDF_compute2c(GTFDF_t df, int tid, int P, int Q,
                             double **ints, int *nints);

int GTFDF_nPriShells(GTFDF_t df);
int GTFDF_nPriFuncs(GTFDF_t df);
int GTFDF_priShellDim(GTFDF_t df, int shell);
int GTFDF_priFuncStart(GTFDF_t df, int shell);

int GTFDF_nAuxShells(GTFDF_t df);
int GTFDF_nAuxFuncs(GTFDF_t df);
int GTFDF_auxShellDim(GTFDF_t df, int shell);
int GTFDF_auxFuncStart(GTFDF_t df, int shell);

/*
 * Simint primitive screening. Defaults to SIMINT_SCREEN_FASTSCHWARZ with a
 * tolerance of 1e-14, matching libcint. Call before any compute: the shell-pair
 * tables are built at create time and are not rebuilt.
 */
double GTFDF_getScreenTol(GTFDF_t df);
CIntStatus_t GTFDF_setScreenTol(GTFDF_t df, double tol);

#ifdef __cplusplus
}
#endif

#endif /* GTFOCK_DF_H */
