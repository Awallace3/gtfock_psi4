/*
 * Integral-layer checks for the density-fitting engine in src/gtfock_df.c.
 *
 * Neither check reads stored reference data (see docs/distributed-df.md):
 *
 *   1. Two-center (P|Q) over uncontracted s shells against the closed form.
 *      This pins the zero shell and the whole normalization chain.
 *   2. Resolution-of-the-identity reconstruction of the exact four-center
 *      integrals from libcint. The auxiliary set is chosen to span the primary
 *      products *exactly*, so Coulomb fitting is exact and the comparison runs
 *      at machine precision rather than at fitting accuracy.
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <CInt.h>

#include "gtfock_df.h"

/* M_PI is not in strict ISO C. */
#define GTFDF_PI 3.14159265358979323846

static int failures = 0;

static void report(const char *what, double err, double tol)
{
    int ok = (err <= tol) && !isnan(err);
    printf("%-46s max err %.3e  tol %.1e  %s\n", what, err, tol,
           ok ? "ok" : "FAILED");
    if (!ok) failures++;
}

/* ---------------------------------------------------------------- helpers */

static BasisSet_t make_basis(int natoms, int *Z, double *X, double *Y,
                             double *Zc, int *shells_p_atom, int nshells,
                             int *L, int *prims_p_shell, double *cc,
                             double *alpha)
{
    int nprims = 0;
    for (int i = 0; i < nshells; i++) nprims += prims_p_shell[i];

    BasisSet_t basis;
    if (CInt_createBasisSet(&basis) != CINT_STATUS_SUCCESS) {
        fprintf(stderr, "CInt_createBasisSet failed\n");
        exit(1);
    }
    if (CInt_importBasisSet(basis, natoms, Z, X, Y, Zc, nprims, nshells,
                            0 /* Cartesian */, shells_p_atom, prims_p_shell,
                            L, cc, alpha) != CINT_STATUS_SUCCESS) {
        fprintf(stderr, "CInt_importBasisSet failed\n");
        exit(1);
    }
    return basis;
}

/* Gauss-Jordan with partial pivoting; n is small (11 in this file). */
static void invert(double *a, int n)
{
    double *inv = (double *) calloc((size_t) n * n, sizeof(double));
    for (int i = 0; i < n; i++) inv[i * n + i] = 1.0;

    for (int col = 0; col < n; col++) {
        int piv = col;
        for (int r = col + 1; r < n; r++)
            if (fabs(a[r * n + col]) > fabs(a[piv * n + col])) piv = r;
        if (fabs(a[piv * n + col]) < 1e-14) {
            fprintf(stderr, "singular Coulomb metric at column %d\n", col);
            exit(1);
        }
        if (piv != col) {
            for (int c = 0; c < n; c++) {
                double t = a[col * n + c]; a[col * n + c] = a[piv * n + c]; a[piv * n + c] = t;
                t = inv[col * n + c]; inv[col * n + c] = inv[piv * n + c]; inv[piv * n + c] = t;
            }
        }
        double d = 1.0 / a[col * n + col];
        for (int c = 0; c < n; c++) { a[col * n + c] *= d; inv[col * n + c] *= d; }
        for (int r = 0; r < n; r++) {
            if (r == col) continue;
            double f = a[r * n + col];
            if (f == 0.0) continue;
            for (int c = 0; c < n; c++) {
                a[r * n + c]   -= f * a[col * n + c];
                inv[r * n + c] -= f * inv[col * n + c];
            }
        }
    }
    memcpy(a, inv, sizeof(double) * (size_t) n * n);
    free(inv);
}

/* ------------------------------------------------- test 1: analytic (P|Q) */

/* Boys function of order zero. */
static double boys0(double x)
{
    if (x < 1e-14) return 1.0 - x / 3.0;
    return 0.5 * sqrt(GTFDF_PI / x) * erf(sqrt(x));
}

/*
 * (P|Q) for two normalized uncontracted s Gaussians. The Cartesian import path
 * in libcint leaves the contraction coefficient alone, so the only
 * normalization applied is Simint's, which for l = 0 and one primitive is
 * exactly (2a/pi)^(3/4).
 */
static double analytic_ss(double a, double b, double R2)
{
    double na  = pow(2.0 * a / GTFDF_PI, 0.75);
    double nb  = pow(2.0 * b / GTFDF_PI, 0.75);
    double rho = a * b / (a + b);
    return na * nb * 2.0 * pow(GTFDF_PI, 2.5) / (a * b * sqrt(a + b)) *
           boys0(rho * R2);
}

static void test_two_center_analytic(void)
{
    /* Trivial primary basis: this check touches only the auxiliary side. */
    int    p_Z[1] = {1};
    double p_X[1] = {0.0}, p_Y[1] = {0.0}, p_Zc[1] = {0.0};
    int    p_spa[1] = {1}, p_L[1] = {0}, p_pps[1] = {1};
    double p_cc[1] = {1.0}, p_al[1] = {1.0};
    BasisSet_t primary = make_basis(1, p_Z, p_X, p_Y, p_Zc, p_spa, 1,
                                    p_L, p_pps, p_cc, p_al);

    /* Three uncontracted s shells on three distinct centers. */
    enum { NA = 3 };
    int    a_Z[NA]  = {1, 1, 1};
    double a_X[NA]  = {0.0,  0.0,  0.7};
    double a_Y[NA]  = {0.0,  0.0, -0.4};
    double a_Zc[NA] = {0.0,  1.3,  0.5};
    int    a_spa[NA] = {1, 1, 1};
    int    a_L[NA]   = {0, 0, 0};
    int    a_pps[NA] = {1, 1, 1};
    double a_cc[NA]  = {1.0, 1.0, 1.0};
    double a_al[NA]  = {2.2, 0.85, 0.31};
    BasisSet_t aux = make_basis(NA, a_Z, a_X, a_Y, a_Zc, a_spa, NA,
                                a_L, a_pps, a_cc, a_al);

    GTFDF_t df;
    if (GTFDF_create(primary, aux, 1, &df) != CINT_STATUS_SUCCESS) {
        fprintf(stderr, "GTFDF_create failed\n");
        exit(1);
    }
    GTFDF_setScreenTol(df, 0.0);

    double max_err = 0.0;
    for (int p = 0; p < NA; p++) {
        for (int q = 0; q < NA; q++) {
            double *ints; int nints;
            if (GTFDF_compute2c(df, 0, p, q, &ints, &nints) != CINT_STATUS_SUCCESS) {
                fprintf(stderr, "GTFDF_compute2c failed\n");
                exit(1);
            }
            if (nints != 1) {
                fprintf(stderr, "(%d|%d) returned %d integrals, expected 1\n",
                        p, q, nints);
                failures++;
                continue;
            }
            double dx = a_X[p] - a_X[q];
            double dy = a_Y[p] - a_Y[q];
            double dz = a_Zc[p] - a_Zc[q];
            double ref = analytic_ss(a_al[p], a_al[q], dx*dx + dy*dy + dz*dz);
            double err = fabs(ints[0] - ref) / fabs(ref);
            if (err > max_err) max_err = err;
        }
    }
    report("two-center (P|Q) vs analytic s Gaussians", max_err, 1e-12);

    GTFDF_destroy(df);
    CInt_destroyBasisSet(aux);
    CInt_destroyBasisSet(primary);
}

/* ---------------------------------------------- test 2: RI reconstruction */

/*
 * Primary basis: one s primitive on A, one p primitive on B. Their products
 * are, exactly:
 *   s_A s_A -> s at A with exponent 2a
 *   s_A p_B -> s and p at the Gaussian product center with exponent a+b
 *   p_B p_B -> Cartesian d at B with exponent 2b
 * The auxiliary basis below is that list, so the Coulomb fit has zero error and
 * any deviation is a bug in the integral layer, not fitting error.
 */
static void test_ri_reconstruction(void)
{
    const double a = 1.1, b = 0.6, zB = 1.3;
    const double zP = b * zB / (a + b);

    int    p_Z[2]  = {1, 1};
    double p_X[2]  = {0.0, 0.0}, p_Y[2] = {0.0, 0.0};
    double p_Zc[2] = {0.0, zB};
    int    p_spa[2] = {1, 1}, p_L[2] = {0, 1}, p_pps[2] = {1, 1};
    double p_cc[2]  = {1.0, 1.0};
    double p_al[2]  = {a, b};
    BasisSet_t primary = make_basis(2, p_Z, p_X, p_Y, p_Zc, p_spa, 2,
                                    p_L, p_pps, p_cc, p_al);

    int    a_Z[3]  = {1, 1, 1};
    double a_X[3]  = {0.0, 0.0, 0.0}, a_Y[3] = {0.0, 0.0, 0.0};
    double a_Zc[3] = {0.0, zP, zB};
    int    a_spa[3] = {1, 2, 1};
    int    a_L[4]   = {0, 0, 1, 2};
    int    a_pps[4] = {1, 1, 1, 1};
    double a_cc[4]  = {1.0, 1.0, 1.0, 1.0};
    double a_al[4]  = {2.0 * a, a + b, a + b, 2.0 * b};
    BasisSet_t aux = make_basis(3, a_Z, a_X, a_Y, a_Zc, a_spa, 4,
                                a_L, a_pps, a_cc, a_al);

    GTFDF_t df;
    if (GTFDF_create(primary, aux, 1, &df) != CINT_STATUS_SUCCESS) {
        fprintf(stderr, "GTFDF_create failed\n");
        exit(1);
    }
    GTFDF_setScreenTol(df, 0.0);

    const int nbf  = GTFDF_nPriFuncs(df);
    const int naux = GTFDF_nAuxFuncs(df);
    const int nsp  = GTFDF_nPriShells(df);
    const int nsa  = GTFDF_nAuxShells(df);
    if (nbf != 4 || naux != 11) {
        fprintf(stderr, "unexpected sizes nbf=%d naux=%d\n", nbf, naux);
        exit(1);
    }

    /* Coulomb metric. */
    double *J = (double *) calloc((size_t) naux * naux, sizeof(double));
    for (int P = 0; P < nsa; P++) {
        for (int Q = 0; Q < nsa; Q++) {
            double *ints; int nints;
            GTFDF_compute2c(df, 0, P, Q, &ints, &nints);
            if (nints == 0) continue;
            int dP = GTFDF_auxShellDim(df, P), sP = GTFDF_auxFuncStart(df, P);
            int dQ = GTFDF_auxShellDim(df, Q), sQ = GTFDF_auxFuncStart(df, Q);
            for (int p = 0; p < dP; p++)
                for (int q = 0; q < dQ; q++)
                    J[(sP + p) * naux + (sQ + q)] = ints[p * dQ + q];
        }
    }
    double sym = 0.0;
    for (int p = 0; p < naux; p++)
        for (int q = 0; q < naux; q++)
            sym = fmax(sym, fabs(J[p * naux + q] - J[q * naux + p]));
    report("Coulomb metric symmetry", sym, 1e-13);

    /* Three-center tensor. */
    double *A3 = (double *) calloc((size_t) naux * nbf * nbf, sizeof(double));
    for (int P = 0; P < nsa; P++) {
        int dP = GTFDF_auxShellDim(df, P), sP = GTFDF_auxFuncStart(df, P);
        for (int M = 0; M < nsp; M++) {
            int dM = GTFDF_priShellDim(df, M), sM = GTFDF_priFuncStart(df, M);
            for (int N = 0; N < nsp; N++) {
                int dN = GTFDF_priShellDim(df, N), sN = GTFDF_priFuncStart(df, N);
                double *ints; int nints;
                GTFDF_compute3c(df, 0, P, M, N, &ints, &nints);
                if (nints == 0) continue;
                for (int p = 0; p < dP; p++)
                    for (int m = 0; m < dM; m++)
                        for (int n = 0; n < dN; n++)
                            A3[((size_t)(sP + p) * nbf + (sM + m)) * nbf + (sN + n)] =
                                ints[(p * dM + m) * dN + n];
            }
        }
    }

    /* Exact four-center reference straight from libcint. */
    SIMINT_t simint;
    if (CInt_createSIMINT(primary, &simint, 1) != CINT_STATUS_SUCCESS) {
        fprintf(stderr, "CInt_createSIMINT failed\n");
        exit(1);
    }
    size_t n4 = (size_t) nbf * nbf * nbf * nbf;
    double *eri = (double *) calloc(n4, sizeof(double));
    for (int A = 0; A < nsp; A++) {
        int dA = GTFDF_priShellDim(df, A), sA = GTFDF_priFuncStart(df, A);
        for (int B = 0; B < nsp; B++) {
            int dB = GTFDF_priShellDim(df, B), sB = GTFDF_priFuncStart(df, B);
            for (int C = 0; C < nsp; C++) {
                int dC = GTFDF_priShellDim(df, C), sC = GTFDF_priFuncStart(df, C);
                for (int D = 0; D < nsp; D++) {
                    int dD = GTFDF_priShellDim(df, D), sD = GTFDF_priFuncStart(df, D);
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

    /* (mn|rs) ~ sum_PQ (P|mn) [J^-1]_PQ (Q|rs). */
    invert(J, naux);
    size_t n2 = (size_t) nbf * nbf;
    double *JA = (double *) calloc((size_t) naux * n2, sizeof(double));
    for (int P = 0; P < naux; P++)
        for (int Q = 0; Q < naux; Q++) {
            double jpq = J[P * naux + Q];
            if (jpq == 0.0) continue;
            for (size_t k = 0; k < n2; k++)
                JA[P * n2 + k] += jpq * A3[Q * n2 + k];
        }

    double max_err = 0.0, max_val = 0.0;
    for (size_t i = 0; i < n2; i++) {
        for (size_t j = 0; j < n2; j++) {
            double acc = 0.0;
            for (int P = 0; P < naux; P++) acc += A3[P * n2 + i] * JA[P * n2 + j];
            double ref = eri[i * n2 + j];
            max_err = fmax(max_err, fabs(acc - ref));
            max_val = fmax(max_val, fabs(ref));
        }
    }
    printf("largest exact (mn|rs) magnitude: %.6f\n", max_val);
    report("RI reconstruction vs exact four-center", max_err, 1e-9);

    free(JA); free(eri); free(A3); free(J);
    GTFDF_destroy(df);
    CInt_destroyBasisSet(aux);
    CInt_destroyBasisSet(primary);
}

int main(void)
{
    test_two_center_analytic();
    test_ri_reconstruction();
    if (failures) {
        printf("%d density-fitting integral check(s) FAILED\n", failures);
        return 1;
    }
    printf("Density-fitting integral checks passed\n");
    return 0;
}
