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

/* Refuses to compile if the <mpi.h> above is not the header belonging to the
 * MPI library this project was built against. Mixing the two links cleanly and
 * then faults inside MPI_Comm_dup; see docs/hpc-site-mpi.md. */
#include "gtfock_mpi_abi.h"

#include <CInt.h>

#ifdef __cplusplus
extern "C" {
#endif

struct PDF;
typedef struct PDF *PDF_t;

/*
 * Build and distribute the fitted tensor. Collective over comm; every rank
 * must pass identical basis sets. This is the expensive call: it computes all
 * three-center integrals, factors the Coulomb metric, fits, and redistributes.
 *
 * fitting_cond is the relative pivot cutoff used when factoring the Coulomb
 * metric: auxiliary functions whose pivot falls below fitting_cond times the
 * largest are dropped. Pass a non-positive value for the 1e-12 default. This is
 * a pivot criterion, not the relative eigenvalue floor Psi4's
 * DF_FITTING_CONDITION applies; the two keep different function counts when the
 * metric's spectrum straddles the cutoff. See pdf_factor_metric for the
 * measured size of that difference.
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
 * Collective over comm; one MPI_Barrier and one MPI_Allreduce of 2 * nbf^2
 * doubles. See PDF_JKPhase for why the barrier is there.
 */
CIntStatus_t PDF_computeJK(PDF_t pdf, const double *D, const double *Cocc,
                           int nocc, double *J, double *K);

int PDF_nBasisFuncs(PDF_t pdf);
/* Auxiliary basis functions the caller supplied. */
int PDF_nAuxFuncs(PDF_t pdf);
/*
 * Fitted tensor rows retained after the metric factorization, summed over
 * ranks. Equals PDF_nAuxFuncs() minus PDF_nMetricNullVectors().
 */
int PDF_nFitFuncs(PDF_t pdf);
/* Fitted tensor rows owned by this rank after redistribution. */
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

/*
 * Wall seconds this rank spent in each phase of PDF_create, for attributing
 * setup cost. Phases are disjoint and sum to slightly less than the call, the
 * remainder being allocation and partitioning. Every rank measures its own
 * elapsed time, including whatever it spent waiting in a collective, so the
 * spread across ranks is the load imbalance.
 */
typedef enum
{
    PDF_PHASE_METRIC = 0, /* two-center integrals and their reduction */
    PDF_PHASE_FACTOR,     /* pivoted Cholesky of the metric, replicated */
    PDF_PHASE_INT3C,      /* three-center integrals over this rank's pairs */
    PDF_PHASE_FIT,        /* pivot reorder and triangular solve, in place */
    PDF_PHASE_REDIST,     /* redistribution onto the auxiliary axis */
    PDF_NPHASES
} PDF_Phase;

double PDF_phaseSeconds(PDF_t pdf, PDF_Phase phase);
/* A stable short name for each phase; NULL for an out-of-range value. */
const char *PDF_phaseName(PDF_Phase phase);

/*
 * Wall seconds this rank has accumulated in each part of PDF_computeJK, summed
 * over every call so far. Together they account for the whole call to within
 * the two memcpys of the result.
 *
 * A plain Allreduce would fold two unrelated costs into one number: moving
 * 2 * nbf^2 doubles, and waiting for whichever rank reached the call last. Those
 * scale differently and have different fixes, and telling them apart is the
 * whole point of measuring here, so a barrier drains the skew into its own
 * clock first. It is not free, but it costs one empty collective against an
 * Allreduce of megabytes, and the sum of the two is what an unbarriered
 * Allreduce would have cost anyway.
 */
typedef enum
{
    PDF_JK_LOCAL = 0, /* contracting this rank's tensor rows against D and Cocc */
    PDF_JK_SKEW,      /* barrier: waiting for the last rank to finish its rows */
    PDF_JK_COMM,      /* the Allreduce itself, with the skew already drained */
    PDF_NJKPHASES
} PDF_JKPhase;

double PDF_jkPhaseSeconds(PDF_t pdf, PDF_JKPhase phase);
/* A stable short name for each part; NULL for an out-of-range value. */
const char *PDF_jkPhaseName(PDF_JKPhase phase);
/* Calls to PDF_computeJK that reached the reduction, for a per-call average. */
int PDF_jkCalls(PDF_t pdf);

#ifdef __cplusplus
}
#endif

#endif /* GTFOCK_PDF_H */
