#pragma once
#include <stdint.h>
#include <Arduino.h>

// Fixed point math helpers
// We use 0-255 for storage, but calculations might need more precision.

struct CurvePoint
{
    uint8_t x;
    uint8_t y;
};

// A segment is defined by P_start, P_control, P_end.
// P_start is points[i], P_end is points[i+1], P_control is controls[i].
struct Curve
{
    uint8_t count; // Number of points. Min 2 (1 segment), Max 4 (3 segments).
    CurvePoint points[4];
    CurvePoint controls[3];
};

class CurveEvaluator
{
public:
    // Evaluate the curve at input x (0-255). Returns y (0-255).
    static uint8_t eval(const Curve &curve, uint8_t x);

private:
    static uint8_t evalSegment(const CurvePoint &p0, const CurvePoint &c0, const CurvePoint &p1, uint8_t x);
    static int32_t solveT(int32_t x0, int32_t cx, int32_t x1, int32_t x_in);
    static int32_t bezier1D(int32_t p0, int32_t c, int32_t p1, int32_t t_1024);
    static uint32_t isqrt(uint32_t n);
};
