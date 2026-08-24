#include <math.h>
#include <stdio.h>
#include <omp.h>

#include "gtfock_atomic.h"

int main(void)
{
    double value = 0.0;
    atomic_add_f64(&value, 1.25);
    if (value != 1.25) {
        fprintf(stderr, "first atomic add produced %.17g, expected 1.25\n", value);
        return 1;
    }

    const int additions = 4096;
    #pragma omp parallel for
    for (int i = 0; i < additions; i++)
        atomic_add_f64(&value, 0.125);

    const double expected = 1.25 + additions * 0.125;
    if (value != expected) {
        fprintf(stderr, "concurrent atomic sum produced %.17g, expected %.17g\n",
                value, expected);
        return 2;
    }
    return 0;
}
