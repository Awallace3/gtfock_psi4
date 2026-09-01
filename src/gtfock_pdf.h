/*
 * Distributed density-fitted Coulomb and exchange matrices.
 *
 * The fitted tensor B[Q][mn] is built once, distributed over MPI ranks by
 * auxiliary function Q, and then contracted against a density each SCF
 * iteration. See docs/distributed-df.md for why Q is the distribution axis and
 * what the three setup phases cost.
 */

#ifndef GTFOCK_PDF_H
#define GTFOCK_PDF_H

/* CInt.h uses NULL without including <stddef.h>; pull it in first so this
 * header stays self-contained for consumers. */
#include <stddef.h>

#include <mpi.h>

#include <CInt.h>

#ifdef __cplusplus
extern "C" {
#endif

struct PDF;
typedef struct PDF *PDF_t;

/*
 * Build and distribute the fitted tensor. Collective over comm; every rank
 * must pass identical basis sets. This is the expensive call: it computes all
 * three-center integrals, forms J^-1/2, and redistributes.
 *
 * fitting_cond is the relative eigenvalue cutoff used when inverting the
 * Coulomb metric; pass a non-positive value for the 1e-12 default.
 *
 * nthreads <= 0 means omp_get_max_threads().
 */
CIntStatus_t PDF_create(MPI_Comm comm, BasisSet_t primary, BasisSet_t auxiliary,
                        int nthreads, double fitting_cond, PDF_t *pdf);
CIntStatus_t PDF_destroy(PDF_t pdf);

/*
 * One SCF iteration's J and K.
 *
 * D    is the nbf x nbf density, row-major, replicated and identical on every
 *      rank. Required when J is requested.
 * Cocc is nbf x nocc, row-major, replicated: the occupied orbital
 *      coefficients with any occupation factor already folded in, so that
 *      D = Cocc Cocc^T. Required when K is requested.
 * J, K are nbf x nbf row-major outputs, overwritten and fully replicated on
 *      return. Either may be NULL to skip that matrix.
 *
 * Collective over comm; one MPI_Allreduce of 2 * nbf^2 doubles.
 */
CIntStatus_t PDF_computeJK(PDF_t pdf, const double *D, const double *Cocc,
                           int nocc, double *J, double *K);

int PDF_nBasisFuncs(PDF_t pdf);
int PDF_nAuxFuncs(PDF_t pdf);
/* Auxiliary functions owned by this rank after redistribution. */
int PDF_nLocalAuxFuncs(PDF_t pdf);
/* Auxiliary functions dropped by the fitting-condition cutoff, on every rank. */
int PDF_nMetricNullVectors(PDF_t pdf);
/* Doubles held in this rank's slice of the fitted tensor. */
size_t PDF_localTensorSize(PDF_t pdf);
/*
 * AO-pair elements this rank computed three-center integrals for, before the
 * redistribution. Zero is legal: with few shells and many ranks the
 * AO-element partition can leave a rank with no shell pairs.
 */
int PDF_nLocalPairElements(PDF_t pdf);

#ifdef __cplusplus
}
#endif

#endif /* GTFOCK_PDF_H */
