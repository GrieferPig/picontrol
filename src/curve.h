#pragma once
#include <stdint.h>

// Curve shape defined by a single parameter h (Q15 fixed-point).
// Formula: y = x*h / (x*h + (1-x)*(1-h))
// h = 16384 (0.5) → linear, h < 16384 → concave, h > 16384 → convex
// Q15: 1.0 = 32768

#pragma pack(push, 1)

struct Curve
{
    int16_t h; // Shape parameter Q15: 16384 = linear
};

#pragma pack(pop)

class CurveEvaluator
{
public:
    // Evaluate the curve at input x (0-255). Returns y (0-255).
    static uint8_t eval(const Curve &curve, uint8_t x);
    // Evaluate the curve at input x (0-1023). Returns y (0-1023).
    static uint16_t eval10(const Curve &curve, uint16_t x);
};
