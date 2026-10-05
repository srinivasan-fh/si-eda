// SiEDA Core — power-integrity planning beyond the lumped PDN (PowerIntegrity.hpp):
//
//   Plane cavity   The rail / ground plane pair as a rectangular cavity (the rail pour's bounding box, plate spacing d):
//                  the impedance between ports at (x_i, y_i) is the modal sum of Lei, Mittra & Wang (IEEE Trans. AdvP
//                  1999) / Novak,  Z_ij = jωμd/(ab) Σ_mn χ_m²χ_n² cos(k_m x_i) cos(k_n y_i) cos(k_m x_j) cos(k_n y_j)
//                  · sinc(k_m w/2) sinc(k_n w/2) / (k_mn² − k²),  k² = ω² μ ε0 εr (1 − j(tan δ + δ_s/d)).
//                  The (0,0) term is the plate capacitance; the others are the cavity modes f_mn = c/(2√εr)·√((m/a)² +
//                  (n/b)²). High-order modes beyond the dynamic set enter as their static (inductive) sum. Every
//                  decoupling capacitor and the regulator connect at their own positions, so the impedance seen at the
//                  loads shows the cavity resonances and the effect of where the capacitors sit.
//   Decap plan     Greedy optimisation of the lumped PDN: at each step the capacitor from a standard set (1 nF 0201 …
//                  47 µF 1206) that lowers the worst |Z| / target ratio below fMax most, until the rail is compliant
//                  (≤ 40 parts), using the rail's typical mounting inductance.
//   IR-drop map    The DC solution of every pour cell and track section with its current density, for the board
//                  heat map.
//
// Limits: rectangular cavity (non-rectangular pours use their bounding box), one plane pair per rail, no package or die.
#pragma once

#include <string>
#include <vector>

#include "sieda/Channel.hpp"
#include "sieda/Json.hpp"
#include "sieda/PowerIntegrity.hpp"

namespace sieda {

class Project;

struct CavityPort {
    double x = 0, y = 0;  // mm, relative to the cavity's corner
    double width = 0.5;   // mm (square port)
};

/// Port impedance matrix of a rectangular plane pair a × b mm, spacing d mm, at f (Hz). `modes` per axis in the
/// dynamic sum (static residual up to 6×).
CMat cavityImpedance(double aMm, double bMm, double dMm, double er, double tanD, const std::vector<CavityPort>& ports, double f,
                     int modes = 30, double copperMm = 0.035);
/// Resonance f_mn of the cavity (Hz).
double cavityModeFrequency(double aMm, double bMm, double er, int m, int n);

struct PdnCavityResult {
    bool available = false;
    std::string note;
    double a = 0, b = 0, d = 0, er = 0;  // mm
    double x0 = 0, y0 = 0;               // cavity corner on the board
    Vec2 observe;                        // where |Z| is evaluated (the loads' centre)
    struct Mode {
        int m = 0, n = 0;
        double f = 0;
    };
    std::vector<Mode> modes;            // first resonances
    std::vector<double> freq, zCavity, zLumped;
    double worstRatio = 0, worstF = 0;  // highest |Z| / target of the cavity curve up to 1 GHz
    int ports = 0;
    std::vector<std::string> recommendations;
};
PdnCavityResult pdnCavity(const Project& project, const PdnRailResult& rail);

struct PdnDecapPlan {
    bool needed = false, compliant = false;
    double worstBefore = 0, worstAfter = 0;  // |Z| / target within the band
    double mounting = 0;                      // H assumed per added capacitor
    struct Add {
        std::string value, footprint;
        double c = 0;
        int count = 0;
    };
    std::vector<Add> additions;
    std::vector<double> freq, zBefore, zAfter;
};
PdnDecapPlan pdnDecapPlan(const PdnRailResult& rail);

/// {"rail","available","note","a","b","d","er","x0","y0","observe":{x,y},"modes":[{m,n,f}],"freq":[…],"zCavity":[…],
/// "zLumped":[…],"target","worstRatio","worstF","recommendations":[…]} for the rail named `net`.
Json pdnCavityJson(const Project& project, const std::string& net);
/// {"rail","needed","compliant","worstBefore","worstAfter","mounting","additions":[{value,footprint,c,count}],
/// "freq":[…],"zBefore":[…],"zAfter":[…],"target"}.
Json pdnDecapPlanJson(const Project& project, const std::string& net);
/// {"rail","analyzed","note","voltage","limit","worst","maxDensity","board":{width,height,outline:[{x,y}]},
/// "cells":[{x,y,size,layer,drop,density}],"segments":[{ax,ay,bx,by,layer,width,current,density,drop}],
/// "loads":[{ref,pin,x,y,drop}],"source":{x,y},"hotspots":[{x,y,density,layer}]}.
Json pdnIrMapJson(const Project& project, const std::string& net);

}  // namespace sieda
