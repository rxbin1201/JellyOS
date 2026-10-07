/*
 * libc: sin, cos, sqrt, fabs, floor.
 *
 * sin and cos reduce the argument to [-pi/2, pi/2] and evaluate the Taylor
 * series up to x^15 there (error below 1e-10): plenty for audio, not a
 * replacement for a real libm with huge arguments.
 */

#include <math.h>

double fabs(double x)
{
    return x < 0 ? -x : x;
}

double floor(double x)
{
    if (x != x || fabs(x) >= 9007199254740992.0) /* NaN, or already an integer */
        return x;
    double truncated = (double)(long long)x;
    return truncated > x ? truncated - 1.0 : truncated;
}

double sqrt(double x)
{
    double result;
    __asm__("sqrtsd %1, %0" : "=x"(result) : "x"(x));
    return result;
}

double sin(double x)
{
    static const double coefficients[] = { -1.0 / 6,           1.0 / 120,           -1.0 / 5040,          1.0 / 362880,
                                           -1.0 / 39916800,    1.0 / 6227020800.0,  -1.0 / 1307674368000.0 };

    if (x != x)
        return x;
    /* Into [-pi, pi], then mirror into [-pi/2, pi/2]: sin(pi - x) = sin(x). */
    x -= 2 * M_PI * floor(x / (2 * M_PI) + 0.5);
    if (x > M_PI / 2)
        x = M_PI - x;
    else if (x < -M_PI / 2)
        x = -M_PI - x;

    double square = x * x, power = x, sum = x;
    for (unsigned i = 0; i < sizeof(coefficients) / sizeof(coefficients[0]); i++) {
        power *= square;
        sum += power * coefficients[i];
    }
    return sum;
}

double cos(double x)
{
    return sin(x + M_PI / 2);
}
