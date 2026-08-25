#pragma once

/*
 * Intel classic's _castf64_u64 intrinsic was a bit cast. IntelLLVM does not
 * provide it; use the compiler's generic atomic operations directly on the
 * double representation rather than a value-converting integer cast.
 */
static inline void atomic_add_f64(volatile double *global_value, double addend)
{
    double expected;
    double desired;
    __atomic_load((double *)global_value, &expected, __ATOMIC_RELAXED);
    do {
        desired = expected + addend;
    } while (!__atomic_compare_exchange((double *)global_value, &expected,
                                        &desired, 0, __ATOMIC_RELAXED,
                                        __ATOMIC_RELAXED));
}
