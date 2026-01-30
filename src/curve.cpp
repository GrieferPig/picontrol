#include "curve.h"

// Integer square root
uint32_t CurveEvaluator::isqrt(uint32_t n)
{
    if (n < 2)
        return n;
    uint32_t smallCandidate = isqrt(n >> 2) << 1;
    uint32_t largeCandidate = smallCandidate + 1;
    if (largeCandidate * largeCandidate > n)
        return smallCandidate;
    else
        return largeCandidate;
}

// Evaluate 1D quadratic bezier at t (0-1024 scale)
// B(t) = (1-t)^2 P0 + 2(1-t)t P1 + t^2 P2
int32_t CurveEvaluator::bezier1D(int32_t p0, int32_t c, int32_t p1, int32_t t)
{
    int32_t invT = 1024 - t;
    // We need to be careful about overflow.
    // p0, c, p1 are 0-255.
    // t is 0-1024.
    // Max val: 255 * 1024 * 1024 ~ 268 million. Fits in int32.

    int32_t term1 = (invT * invT * p0);
    int32_t term2 = (2 * invT * t * c);
    int32_t term3 = (t * t * p1);

    return (term1 + term2 + term3 + 524288) >> 20; // Divide by 1024*1024 (2^20)
}

// Solve for t (0-1024) given x_in
// x(t) = (x0 - 2cx + x1)t^2 + (2cx - 2x0)t + x0 = x_in
// At^2 + Bt + C = 0
// A = x0 - 2cx + x1
// B = 2cx - 2x0
// C = x0 - x_in
int32_t CurveEvaluator::solveT(int32_t x0, int32_t cx, int32_t x1, int32_t x_in)
{
    int32_t A = x0 - 2 * cx + x1;
    int32_t B = 2 * cx - 2 * x0;
    int32_t C = x0 - x_in;

    if (A == 0)
    {
        // Linear case: Bt + C = 0 -> t = -C / B
        if (B == 0)
            return 0; // Should not happen for valid segment
        return (-C * 1024) / B;
    }

    // Quadratic formula: t = (-B +/- sqrt(B^2 - 4AC)) / 2A
    // We want t in [0, 1024].
    // Since x(t) is monotonic, there should be one valid solution.

    int32_t delta = B * B - 4 * A * C;
    if (delta < 0)
        return 0; // No solution?

    int32_t sqrtDelta = isqrt(delta);

    // Try both solutions
    int32_t t1 = (-B + sqrtDelta) * 1024 / (2 * A);
    int32_t t2 = (-B - sqrtDelta) * 1024 / (2 * A);

    if (t1 >= 0 && t1 <= 1024)
        return t1;
    if (t2 >= 0 && t2 <= 1024)
        return t2;

    // Clamp if slightly out due to precision
    if (t1 < 0 && t1 > -50)
        return 0;
    if (t1 > 1024 && t1 < 1074)
        return 1024;

    return (t1 > 0) ? t1 : 0; // Fallback
}

uint8_t CurveEvaluator::evalSegment(const CurvePoint &p0, const CurvePoint &c0, const CurvePoint &p1, uint8_t x)
{
    // If x is outside x-bounds of segment, clamp?
    // The caller should ensure x is within [p0.x, p1.x] roughly.

    // Solve for t
    int32_t t = solveT(p0.x, c0.x, p1.x, x);

    // Compute y(t)
    int32_t y = bezier1D(p0.y, c0.y, p1.y, t);

    if (y < 0)
        y = 0;
    if (y > 255)
        y = 255;
    return (uint8_t)y;
}

uint8_t CurveEvaluator::eval(const Curve &curve, uint8_t x)
{
    if (curve.count < 2)
        return x; // Fallback linear

    // Find segment
    // We assume points are sorted by x.
    for (int i = 0; i < curve.count - 1; i++)
    {
        // Check if x is in this segment [points[i].x, points[i+1].x]
        // We use < for the last point check to cover the exact end.
        if (x <= curve.points[i + 1].x)
        {
            return evalSegment(curve.points[i], curve.controls[i], curve.points[i + 1], x);
        }
    }

    // If x is beyond the last point, clamp to last point's y?
    // Or extrapolate? Clamping is safer for mapping.
    return curve.points[curve.count - 1].y;
}
