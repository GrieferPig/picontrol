#include "curve.h"

uint8_t CurveEvaluator::eval(const Curve &curve, uint8_t x)
{
    // Boundary cases: always pass through (0,0) and (255,255)
    if (x == 0)
        return 0;
    if (x == 255)
        return 255;

    // Convert x from 0-255 to Q15 (0-32768)
    int32_t X = ((int32_t)x * 32768 + 127) / 255;
    int32_t H = curve.h;
    int32_t ONE = 32768;

    // Formula: y = x*h / (x*h + (1-x)*(1-h))
    int32_t term1 = X * H;                 // x*h
    int32_t term2 = (ONE - X) * (ONE - H); // (1-x)*(1-h)

    int32_t denominator = term1 + term2;

    if (denominator == 0)
        return 0;

    int32_t result = (term1 * ONE) / denominator;

    // Clamp to valid range
    if (result < 0)
        result = 0;
    if (result > 32768)
        result = 32768;

    // Convert back from Q15 to 0-255
    return (uint8_t)((result * 255 + 16384) / 32768);
}
