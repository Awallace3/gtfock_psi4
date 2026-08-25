#pragma once

#include <mm_malloc.h>
#include <stdint.h>
#include <string.h>

/* The legacy tree redeclares ScaLAPACK helpers with pre-const signatures. */
#define _MKL_SCALAPACK_H_
extern void pdsyevd_(const char *jobz, const char *uplo, const int *n,
                     const double *a, const int *ia, const int *ja,
                     const int *desca, double *w, double *z, const int *iz,
                     const int *jz, const int *descz, double *work,
                     const int *lwork, int *iwork, const int *liwork,
                     int *info);

/* Read-only legacy comparison support: Intel classic provided this bit cast. */
static inline uint64_t _castf64_u64(double value)
{
    uint64_t bits;
    memcpy(&bits, &value, sizeof(bits));
    return bits;
}
