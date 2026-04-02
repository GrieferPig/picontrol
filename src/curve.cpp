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
    const int64_t ONE = 32768;

    // Use 64-bit temporaries to avoid overflow in intermediate multiplications.
    // Formula: y = x*h / (x*h + (1-x)*(1-h))
    int64_t X64 = X;
    int64_t H64 = (int64_t)(uint16_t)H; // treat stored int16_t as unsigned Q15 value

    int64_t term1 = X64 * H64;                 // x*h
    int64_t term2 = (ONE - X64) * (ONE - H64); // (1-x)*(1-h)

    int64_t denominator = term1 + term2;

    if (denominator == 0)
        return 0;

    int64_t result64 = (term1 * ONE) / denominator;

    int32_t result = (int32_t)result64;

    // Clamp to valid range
    if (result < 0)
        result = 0;
    if (result > 32768)
        result = 32768;

    // Convert back from Q15 to 0-255
    return (uint8_t)((result * 255 + 16384) / 32768);
}

uint16_t CurveEvaluator::eval10(const Curve &curve, uint16_t x)
{
    // Boundary cases: always pass through (0,0) and (1023,1023)
    if (x == 0)
        return 0;
    if (x >= 1023)
        return 1023;

    // Convert x from 0-1023 to Q15 (0-32768)
    int32_t X = ((int32_t)x * 32768 + 511) / 1023;
    int32_t H = curve.h;
    const int64_t ONE = 32768;

    // Use 64-bit temporaries to avoid overflow in intermediate multiplications.
    // Formula: y = x*h / (x*h + (1-x)*(1-h))
    int64_t X64 = X;
    int64_t H64 = (int64_t)(uint16_t)H; // treat stored int16_t as unsigned Q15 value

    int64_t term1 = X64 * H64;                 // x*h
    int64_t term2 = (ONE - X64) * (ONE - H64); // (1-x)*(1-h)

    int64_t denominator = term1 + term2;

    if (denominator == 0)
        return 0;

    int64_t result64 = (term1 * ONE) / denominator;

    int32_t result = (int32_t)result64;

    // Clamp to valid range
    if (result < 0)
        result = 0;
    if (result > 32768)
        result = 32768;

    // Convert back from Q15 to 0-1023
    return (uint16_t)((result * 1023 + 16384) / 32768);
}
