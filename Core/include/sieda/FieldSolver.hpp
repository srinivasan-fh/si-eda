// SiEDA Core — 2D quasi-static field solver for track cross-sections. It solves Laplace's equation ∇·(ε∇V) = 0 by
// finite volumes on a graded tensor grid (nodes on every conductor and dielectric edge, cells that grow away from the
// edges), with conjugate gradients, once with the real dielectric and once in air. The conductor charges give the
// capacitance matrices C and C0, then L = μ0ε0·C0⁻¹, Z0 = 1/(c·√(C·C0)), εeff = C/C0 — the method of field-solver
// tools such as Polar Si or HyperLynx, for single tracks and edge-coupled pairs on microstrip or stripline.
#pragma once

#include "sieda/Json.hpp"
#include "sieda/Pcb.hpp"

namespace sieda {

/// One cross-section (mm). Ground plane at y = 0; the track sits on dielectric `h`. `hTop` > 0: a second plane hTop
/// above the track top (stripline, fully embedded); 0: air above (microstrip). `s` > 0: a second track `s` away.
struct FieldGeometry {
    double w = 0.2, t = 0.035, h = 0.2, hTop = 0, s = 0, er = 4.4;
};

struct FieldResult {
    double z0 = 0, eeff = 0, delayPsPerMm = 0, lNhPerMm = 0, cPfPerMm = 0;  // one track alone (odd/even averaged out)
    double zodd = 0, zeven = 0, zdiff = 0, zcommon = 0;                      // pairs only
    double kb = 0, kf = 0;  // backward coupling ¼(Lm/L + Cm/C); forward ½(Cm/C − Lm/L), × TD / RT gives FEXT
    int nodes = 0;
};

FieldResult solveField(const FieldGeometry& g);
/// The cross-section of `layer` from the board's stack-up (laminate εr, copper weight, dielectric heights).
FieldGeometry trackGeometry(const BoardSettings& s, int layer, double w, double gap = 0);
Json fieldResultJson(const FieldGeometry& g, const FieldResult& r);

}  // namespace sieda
