#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include <mpi.h>

#include "CInt.h"
#include "GTMatrix.h"
#include "one_electron.h"
#include "pfock.h"

static const double legacy_overlap[49] = {
    1.00000000000000000e+00,  2.36703936510847618e-01,  0.0,
    0.0,                      3.88320941889785307e-18,  6.17831936607901727e-02,
    6.17831936607901727e-02,  2.36703936510847618e-01,  9.99999999999999889e-01,
    0.0,                      0.0,                     -3.33176972149496417e-17,
    5.13048862922784954e-01,  5.13048862922784954e-01,  0.0,
    0.0,                      9.99999999999999889e-01,  0.0,
    0.0,                      0.0,                      0.0,
    0.0,                      0.0,                      0.0,
    9.99999999999999889e-01,  0.0,                     -3.26167479584910214e-01,
    3.26167479584910214e-01, -6.01688591016878126e-19, -2.54263402677481925e-17,
    0.0,                      0.0,                      9.99999999999999889e-01,
    2.52545915004429689e-01,  2.52545915004429689e-01,  6.17831936607901727e-02,
    5.13048862922784843e-01,  0.0,                     -3.26167479584910269e-01,
    2.52545915004429633e-01,  1.00000000000000022e+00,  2.86265448519778754e-01,
    6.17831936607901727e-02,  5.13048862922784843e-01,  0.0,
    3.26167479584910269e-01,  2.52545915004429633e-01,  2.86265448519778754e-01,
    1.00000000000000022e+00
};

int main(int argc, char **argv)
{
    int provided, rank, size;
    MPI_Init_thread(&argc, &argv, MPI_THREAD_SERIALIZED, &provided);
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &size);
    if (argc != 3 || size != 2 || provided < MPI_THREAD_SERIALIZED) {
        if (rank == 0) fprintf(stderr, "usage: test_overlap <basis> <xyz> (2 ranks)\n");
        MPI_Abort(MPI_COMM_WORLD, 2);
    }

    BasisSet_t basis = NULL;
    PFock_t pfock = NULL;
    if (CInt_createBasisSet(&basis) != CINT_STATUS_SUCCESS ||
        CInt_loadBasisSet(basis, argv[1], argv[2]) != CINT_STATUS_SUCCESS ||
        CInt_getNumFuncs(basis) != 7 ||
        PFock_create(basis, 2, 1, 1, 1.0e-11, 1, 1, &pfock) !=
            PFOCK_STATUS_SUCCESS) {
        MPI_Abort(MPI_COMM_WORLD, 3);
    }

    double zero = 0.0;
    double overlap[49] = {0.0};
    GTM_fill(pfock->gtm_Smat, &zero);
    compute_S(pfock, basis, pfock->sshell_row, pfock->eshell_row,
              pfock->sshell_col, pfock->eshell_col,
              pfock->gtm_Smat->ld_local, pfock->gtm_Smat->mat_block);
    GTM_sync(pfock->gtm_Smat);
    if (rank == 0)
        PFock_getOvlMat(pfock, 0, 6, 0, 6, 7, overlap);
    MPI_Bcast(overlap, 49, MPI_DOUBLE, 0, MPI_COMM_WORLD);

    double max_error = 0.0;
    double max_asymmetry = 0.0;
    for (int i = 0; i < 7; ++i) {
        for (int j = 0; j < 7; ++j) {
            double error = fabs(overlap[i * 7 + j] - legacy_overlap[i * 7 + j]);
            double asymmetry = fabs(overlap[i * 7 + j] - overlap[j * 7 + i]);
            if (error > max_error) max_error = error;
            if (asymmetry > max_asymmetry) max_asymmetry = asymmetry;
        }
    }

    GTM_destroy(pfock->gtm_Hmat);
    GTM_destroy(pfock->gtm_Smat);
    GTM_destroy(pfock->gtm_Xmat);
    GTM_destroy(pfock->gtm_tmp1);
    GTM_destroy(pfock->gtm_tmp2);
    PFock_destroy(pfock);
    CInt_destroyBasisSet(basis);

    int failed = max_error > 1.0e-13 || max_asymmetry > 1.0e-13;
    if (rank == 0)
        printf("legacy overlap max_abs=%.3e symmetry=%.3e\n",
               max_error, max_asymmetry);
    MPI_Finalize();
    return failed;
}
