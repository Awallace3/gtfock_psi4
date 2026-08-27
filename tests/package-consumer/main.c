#include <mpi.h>
#include <pfock.h>

int main(int argc, char **argv)
{
    int initialized = 0;
    BasisSet_t basis = 0;

    MPI_Init(&argc, &argv);
    if (CInt_createBasisSet(&basis) != CINT_STATUS_SUCCESS)
        return 2;
    if (argc == 99)
        (void) PFock_getStatistics((PFock_t) 0);
    if (CInt_destroyBasisSet(basis) != CINT_STATUS_SUCCESS)
        return 3;
    MPI_Initialized(&initialized);
    MPI_Finalize();
    return initialized ? 0 : 4;
}
