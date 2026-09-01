/*
 * Distributed density-fitted J and K, from src/gtfock_pdf.c.
 *
 * The auxiliary basis below spans the primary products *exactly*, so Coulomb
 * fitting is error-free and DF J and K must reproduce the exact four-center
 * matrices to machine precision. That turns what would otherwise be a
 * fitting-accuracy comparison into a real regression test: any deviation is a
 * bug in the three-center layer, the fit, the redistribution, or the
 * contraction, not basis-set error.
 *
 * Run at several rank counts (see CMakeLists.txt): three ranks leaves one rank
 * with no shell pairs at all, which is the interesting edge of the Phase A/C
 * partitioning.
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <mpi.h>

#include <CInt.h>

#include "gtfock_df.h"
#include "gtfock_pdf.h"

static int failures = 0;
static int world_rank = 0;

static void report(const char *what, double err, double tol)
{
    int ok = (err <= tol) && !isnan(err);
    if (world_rank == 0)
        printf("%-46s max err %.3e  tol %.1e  %s\n", what, err, tol,
               ok ? "ok" : "FAILED");
    if (!ok) failures++;
}

static void die(const char *what)
{
    fprintf(stderr, "rank %d: %s failed\n", world_rank, what);
    MPI_Abort(MPI_COMM_WORLD, 1);
    exit(1);
}

static BasisSet_t make_basis(int natoms, int *Z, double *X, double *Y,
                             double *Zc, int *shells_p_atom, int nshells,
                             int *L, int *prims_p_shell, double *cc,
                             double *alpha)
{
    int nprims = 0;
    for (int i = 0; i < nshells; i++) nprims += prims_p_shell[i];

    BasisSet_t basis;
    if (CInt_createBasisSet(&basis) != CINT_STATUS_SUCCESS)
        die("CInt_createBasisSet");
    if (CInt_importBasisSet(basis, natoms, Z, X, Y, Zc, nprims, nshells,
                            0 /* Cartesian */, shells_p_atom, prims_p_shell,
                            L, cc, alpha) != CINT_STATUS_SUCCESS)
        die("CInt_importBasisSet");
    return basis;
}

/* ------------------------------------------------------------- the basis */

/*
 * Primary: one s primitive on A, one p primitive on B, one s primitive on C.
 * Every product of two primary functions is a single Gaussian shell at the
 * corresponding Gaussian product center, and the auxiliary list is exactly
 * that set of shells.
 */
static const double a_exp = 1.1, b_exp = 0.6, c_exp = 0.85;
static const double posA[3] = {0.0,  0.0, 0.0};
static const double posB[3] = {0.0,  0.0, 1.3};
static const double posC[3] = {0.9, -0.5, 0.4};

static void product_center(const double *p, double pe, const double *q,
                           double qe, double *out)
{
    for (int i = 0; i < 3; i++) out[i] = (pe * p[i] + qe * q[i]) / (pe + qe);
}

static BasisSet_t build_primary(void)
{
    int    Z[3]  = {1, 1, 1};
    double X[3]  = {posA[0], posB[0], posC[0]};
    double Y[3]  = {posA[1], posB[1], posC[1]};
    double Zc[3] = {posA[2], posB[2], posC[2]};
    int    spa[3] = {1, 1, 1};
    int    L[3]   = {0, 1, 0};
    int    pps[3] = {1, 1, 1};
    double cc[3]  = {1.0, 1.0, 1.0};
    double al[3]  = {a_exp, b_exp, c_exp};
    return make_basis(3, Z, X, Y, Zc, spa, 3, L, pps, cc, al);
}

static BasisSet_t build_auxiliary(void)
{
    double pAB[3], pAC[3], pBC[3];
    product_center(posA, a_exp, posB, b_exp, pAB);
    product_center(posA, a_exp, posC, c_exp, pAC);
    product_center(posB, b_exp, posC, c_exp, pBC);

    /* Centers: A, P_AB, B, P_AC, C, P_BC. */
    const double *ctr[6] = {posA, pAB, posB, pAC, posC, pBC};
    int    Z[6];
    double X[6], Y[6], Zc[6];
    for (int i = 0; i < 6; i++)
    {
        Z[i] = 1;
        X[i] = ctr[i][0];
        Y[i] = ctr[i][1];
        Zc[i] = ctr[i][2];
    }

    /*  A: s(2a)                        <- s_A s_A
     *  P_AB: s(a+b), p(a+b)            <- s_A p_B
     *  B: d(2b)                        <- p_B p_B
     *  P_AC: s(a+c)                    <- s_A s_C
     *  C: s(2c)                        <- s_C s_C
     *  P_BC: s(b+c), p(b+c)            <- p_B s_C
     */
    int    spa[6] = {1, 2, 1, 1, 1, 2};
    int    L[8]   = {0, 0, 1, 2, 0, 0, 0, 1};
    int    pps[8] = {1, 1, 1, 1, 1, 1, 1, 1};
    double cc[8]  = {1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0};
    double al[8]  = {2.0 * a_exp,
                     a_exp + b_exp, a_exp + b_exp,
                     2.0 * b_exp,
                     a_exp + c_exp,
                     2.0 * c_exp,
                     b_exp + c_exp, b_exp + c_exp};
    return make_basis(6, Z, X, Y, Zc, spa, 8, L, pps, cc, al);
}

/* Dense (mn|rs) straight from libcint, nbf^4 doubles. */
static double *exact_eri(BasisSet_t primary, int nbf)
{
    SIMINT_t simint;
    if (CInt_createSIMINT(primary, &simint, 1) != CINT_STATUS_SUCCESS)
        die("CInt_createSIMINT");

    int nsh = (int) CInt_getNumShells(primary);
    double *eri = (double *) calloc((size_t) nbf * nbf * nbf * nbf,
                                    sizeof(double));
    if (eri == NULL) die("calloc(eri)");

    for (int A = 0; A < nsh; A++)
    {
        int dA = CInt_getShellDim(primary, A), sA = CInt_getFuncStartInd(primary, A);
        for (int B = 0; B < nsh; B++)
        {
            int dB = CInt_getShellDim(primary, B), sB = CInt_getFuncStartInd(primary, B);
            for (int C = 0; C < nsh; C++)
            {
                int dC = CInt_getShellDim(primary, C), sC = CInt_getFuncStartInd(primary, C);
                for (int D = 0; D < nsh; D++)
                {
                    int dD = CInt_getShellDim(primary, D);
                    int sD = CInt_getFuncStartInd(primary, D);
                    double *ints; int nints;
                    CInt_computeShellQuartet_SIMINT(simint, 0, A, B, C, D,
                                                    &ints, &nints);
                    if (nints == 0) continue;
                    int i = 0;
                    for (int p = 0; p < dA; p++)
                      for (int q = 0; q < dB; q++)
                        for (int r = 0; r < dC; r++)
                          for (int s = 0; s < dD; s++, i++)
                            eri[(((size_t)(sA+p) * nbf + (sB+q)) * nbf
                                 + (sC+r)) * nbf + (sD+s)] = ints[i];
                }
            }
        }
    }
    CInt_destroySIMINT(simint, 0);
    return eri;
}

static double maxdiff(const double *x, const double *y, size_t n)
{
    double m = 0.0;
    for (size_t i = 0; i < n; i++) m = fmax(m, fabs(x[i] - y[i]));
    return m;
}

int main(int argc, char **argv)
{
    MPI_Init(&argc, &argv);
    int nranks;
    MPI_Comm_rank(MPI_COMM_WORLD, &world_rank);
    MPI_Comm_size(MPI_COMM_WORLD, &nranks);

    BasisSet_t primary = build_primary();
    BasisSet_t aux = build_auxiliary();

    PDF_t pdf;
    if (PDF_create(MPI_COMM_WORLD, primary, aux, 1, 0.0, &pdf)
        != CINT_STATUS_SUCCESS)
        die("PDF_create");

    const int nbf = PDF_nBasisFuncs(pdf);
    const int naux = PDF_nAuxFuncs(pdf);
    if (nbf != 5 || naux != 17)
    {
        fprintf(stderr, "unexpected sizes nbf=%d naux=%d\n", nbf, naux);
        MPI_Abort(MPI_COMM_WORLD, 1);
    }
    if (world_rank == 0)
        printf("ranks %d, nbf %d, naux %d, dropped aux vectors %d\n",
               nranks, nbf, naux, PDF_nMetricNullVectors(pdf));
    printf("  rank %d: %d AO-pair elements in phase A, %d auxiliary functions "
           "and %zu tensor doubles after\n",
           world_rank, PDF_nLocalPairElements(pdf), PDF_nLocalAuxFuncs(pdf),
           PDF_localTensorSize(pdf));
    fflush(stdout);

    /* The two partitions each cover their axis exactly once. */
    int qsum = PDF_nLocalAuxFuncs(pdf);
    MPI_Allreduce(MPI_IN_PLACE, &qsum, 1, MPI_INT, MPI_SUM, MPI_COMM_WORLD);
    report("auxiliary functions accounted for",
           fabs((double) (qsum - naux)), 0.0);

    /*
     * Shell-pair blocks: (s,s) 1, (p,s) 3, (p,p) 9, (s,s) 1, (s,p) 3, (s,s) 1.
     */
    int aosum = PDF_nLocalPairElements(pdf);
    int aoempty = (aosum == 0) ? 1 : 0;
    MPI_Allreduce(MPI_IN_PLACE, &aosum, 1, MPI_INT, MPI_SUM, MPI_COMM_WORLD);
    MPI_Allreduce(MPI_IN_PLACE, &aoempty, 1, MPI_INT, MPI_SUM, MPI_COMM_WORLD);
    report("AO-pair elements accounted for", fabs((double) (aosum - 18)), 0.0);
    if (nranks == 3)
    {
        /*
         * Pin the case the CMake comment advertises: at three ranks the
         * 9-element (p,p) block cannot be split, so one rank gets nothing and
         * the empty-block paths in Phase A and the redistribution run.
         */
        report("three ranks leave exactly one rank empty",
               fabs((double) (aoempty - 1)), 0.0);
    }

    const int nocc = 2;
    static const double Cocc[5 * 2] = {
         0.31, -0.42,
         0.17,  0.23,
        -0.28,  0.11,
         0.44,  0.36,
        -0.19,  0.27,
    };
    size_t n2 = (size_t) nbf * nbf;
    double *D = (double *) calloc(n2, sizeof(double));
    for (int m = 0; m < nbf; m++)
        for (int n = 0; n < nbf; n++)
            for (int i = 0; i < nocc; i++)
                D[m * nbf + n] += Cocc[m * nocc + i] * Cocc[n * nocc + i];

    double *J = (double *) calloc(n2, sizeof(double));
    double *K = (double *) calloc(n2, sizeof(double));
    if (PDF_computeJK(pdf, D, Cocc, nocc, J, K) != CINT_STATUS_SUCCESS)
        die("PDF_computeJK");

    /* Exact reference. */
    double *eri = exact_eri(primary, nbf);
    double *Jref = (double *) calloc(n2, sizeof(double));
    double *Kref = (double *) calloc(n2, sizeof(double));
    for (int m = 0; m < nbf; m++)
        for (int n = 0; n < nbf; n++)
            for (int r = 0; r < nbf; r++)
                for (int s = 0; s < nbf; s++)
                {
                    Jref[m * nbf + n] +=
                        eri[(((size_t) m * nbf + n) * nbf + r) * nbf + s]
                        * D[r * nbf + s];
                    Kref[m * nbf + n] +=
                        eri[(((size_t) m * nbf + r) * nbf + n) * nbf + s]
                        * D[r * nbf + s];
                }

    report("DF J vs exact four-center J", maxdiff(J, Jref, n2), 1e-9);
    report("DF K vs exact four-center K", maxdiff(K, Kref, n2), 1e-9);

    /* Both outputs are symmetric matrices. */
    double jsym = 0.0, ksym = 0.0;
    for (int m = 0; m < nbf; m++)
        for (int n = 0; n < nbf; n++)
        {
            jsym = fmax(jsym, fabs(J[m * nbf + n] - J[n * nbf + m]));
            ksym = fmax(ksym, fabs(K[m * nbf + n] - K[n * nbf + m]));
        }
    report("J symmetry", jsym, 1e-13);
    report("K symmetry", ksym, 1e-13);

    /* Every rank returns the same matrices, bit for bit after the reduce. */
    double *bcast = (double *) malloc(sizeof(double) * 2 * n2);
    memcpy(bcast, J, sizeof(double) * n2);
    memcpy(bcast + n2, K, sizeof(double) * n2);
    MPI_Bcast(bcast, (int) (2 * n2), MPI_DOUBLE, 0, MPI_COMM_WORLD);
    double repl = fmax(maxdiff(bcast, J, n2), maxdiff(bcast + n2, K, n2));
    report("J and K replicated across ranks", repl, 0.0);

    /* Requesting one matrix must not change the other's value. */
    double *Jonly = (double *) calloc(n2, sizeof(double));
    double *Konly = (double *) calloc(n2, sizeof(double));
    if (PDF_computeJK(pdf, D, NULL, 0, Jonly, NULL) != CINT_STATUS_SUCCESS)
        die("PDF_computeJK(J only)");
    if (PDF_computeJK(pdf, NULL, Cocc, nocc, NULL, Konly) != CINT_STATUS_SUCCESS)
        die("PDF_computeJK(K only)");
    report("J unchanged when K is skipped", maxdiff(Jonly, J, n2), 0.0);
    report("K unchanged when J is skipped", maxdiff(Konly, K, n2), 0.0);

    free(Jonly); free(Konly); free(bcast);
    free(Kref); free(Jref); free(eri);
    free(K); free(J); free(D);

    PDF_destroy(pdf);
    CInt_destroyBasisSet(aux);
    CInt_destroyBasisSet(primary);

    int total = failures;
    MPI_Allreduce(MPI_IN_PLACE, &total, 1, MPI_INT, MPI_SUM, MPI_COMM_WORLD);
    if (world_rank == 0)
    {
        if (total) printf("%d distributed DF check(s) FAILED\n", total);
        else printf("Distributed density-fitted J/K checks passed\n");
    }
    MPI_Finalize();
    return total ? 1 : 0;
}
