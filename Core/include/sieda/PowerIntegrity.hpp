// SiEDA Core — power integrity: the power-distribution network (PDN) of every supply rail against its target
// impedance, and the DC voltage drop (IR drop) across its copper.
//
// Impedance vs frequency is the parallel combination of the regulator (VRM: output resistance and the inductance that
// models its control-loop roll-off), every decoupling capacitor on the rail (C, ESR, ESL of its package plus the
// mounting inductance of its pads, traces and vias) and the plane-pair capacitance where the rail and ground are
// poured on facing layers. The target is Z = V · ripple / I_transient (Smith & Swaminathan; Novak). IR drop solves the
// resistive network of the rail's tracks, vias and poured copper (sheet resistance ρ / t of the copper weight) from
// the source pad to every load pin. Board level only: package and die capacitance, plane spreading / cavity modes
// beyond the first resonance estimate and the ground return's own drop are not modelled.
#pragma once

#include <complex>
#include <string>
#include <vector>

#include "sieda/Json.hpp"
#include "sieda/Pcb.hpp"

namespace sieda {

class Project;

/// Target impedance (Ω): V · ripple% / 100 / I_transient.
double targetImpedance(double volts, double ripplePercent, double transientAmps);
/// Series R-L-C of a capacitor at frequency f (Hz): ESR + jωL + 1/(jωC).
std::complex<double> capacitorImpedance(double f, double c, double esr, double esl);
/// Self-resonant frequency 1 / (2π √(L·C)).
double selfResonance(double c, double l);
/// Parallel-plate capacitance (F) of `areaMm2` at separation `gapMm` with relative permittivity er.
double planeCapacitance(double areaMm2, double gapMm, double er);
/// Copper resistivity at 20 °C (Ω·m), IPC-2152 / annealed copper standard.
constexpr double kCopperResistivity = 1.72e-8;
/// Sheet resistance (Ω/□) of copper `thicknessMm` thick: ρ / t.
double sheetResistance(double thicknessMm);
/// Typical ceramic / tantalum / electrolytic parasitics by footprint (C_0402 … CP_Radial_THT) and value.
struct CapacitorParasitics {
    double esr = 0.03;   // Ω
    double esl = 0.6e-9; // H, the package alone
};
CapacitorParasitics capacitorParasitics(const std::string& footprint, double farads);

struct PdnDecap {
    int componentId = -1;
    std::string ref, value, footprint;
    double c = 0, esr = 0, esl = 0, mounting = 0;  // F, Ω, H, H
    double srf = 0;       // with mounting inductance (Hz)
    double distance = 0;  // to the nearest load pin (mm)
};

struct PdnLoad {
    int componentId = -1;
    std::string ref, pin;
    double current = 0;  // A (DC)
    double drop = 0;     // V below the source pad
    bool connected = true;
};

struct PdnRailResult {
    int net = -1;
    std::string name;
    double voltage = 0;
    bool voltageEstimated = false;
    double ripplePercent = 5;
    double transientCurrent = 0, dcCurrent = 0;
    bool currentEstimated = false;
    double target = 0;   // Ω
    double fMax = 100e6; // upper end of the board-level band (Hz)
    // Elements
    std::string vrmRef, vrmKind;  // "LDO", "switching regulator", "supply", "connector"
    double vrmR = 0, vrmL = 0;
    std::vector<PdnDecap> decaps;
    double planeArea = 0, planeGap = 0, planeC = 0, planeL = 0;  // mm², mm, F, H
    std::string planeLayers;
    double cavityResonance = 0;  // first plane-cavity mode (Hz), 0 without a plane
    // Impedance profile
    std::vector<double> freq, z;  // Hz, |Z| Ω
    struct Peak {
        double f = 0, z = 0;
    };
    std::vector<Peak> peaks;  // anti-resonances (local maxima)
    double worstZ = 0, worstF = 0;  // highest |Z| / target ratio within the band
    bool compliant = true;
    // IR drop
    bool irAnalyzed = false;
    std::string irSource, irNote;
    double irWorst = 0;  // V
    std::string irWorstRef;
    double irLimitPercent = 2.5;
    std::vector<PdnLoad> loads;
    std::vector<std::string> recommendations;
};

/// Every supply rail with loads or decoupling (power and negative nets with two or more pads), using the project's
/// PdnRailSettings overrides.
std::vector<PdnRailResult> analyzePdn(const Project& project);
/// {"rails":[{net,name,voltage,…,curve:{freq:[],z:[]},peaks:[],decaps:[],loads:[],recommendations:[]}]}
Json pdnJson(const std::vector<PdnRailResult>& rails);

}  // namespace sieda
