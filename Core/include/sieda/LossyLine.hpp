// SiEDA Core — frequency-dependent lossy transmission lines: per-unit-length R(f) L(f) G(f) C(f) of every stack-up layer
// from closed-form, causal models.
//
//   Dielectric  Djordjevic–Sarkar wideband Debye model (Djordjevic, Biljic, Likar-Smiljanic, Sarkar, IEEE Trans. EMC
//               43(4), 2001): ε(ω) = ε∞ + Δε/(m2 − m1) · log10((ω2 + jω)/(ω1 + jω)), fitted to the laminate's εr and
//               tan δ at 1 GHz. Causal by construction (Kramers–Kronig consistent): εr falls slowly with frequency and
//               tan δ is nearly flat over the band.
//   Conductor   DC resistance ρ/(w·t) blending into the skin-effect surface resistance Rs = √(π f μ0 ρ) through the causal
//               form Z_int = R_dc·√(1 + jω/ωc) (real and imaginary parts equal at high frequency, i.e. the internal
//               inductance of the skin effect is included). The geometry factor R_ac / Rs comes from Wheeler's
//               incremental-inductance rule: on a stripline Pozar's closed form (Microwave Engineering, 4th ed.,
//               eq. 3.198), on a microstrip the numerical Wheeler derivative of the Hammerstad–Jensen inductance.
//   Roughness   Huray "snowball" model (causal form with the complex skin depth δ(1 − j)/√2) or Hammerstad–Jensen
//               K = 1 + (2/π)·atan(1.4 (Δ/δ)²), applied to the skin-effect part of Z_int. Foil profiles: smooth, HVLP,
//               VLP, RTF, standard ED.
//   L and C     The lossless line of the stack-up (IPC-2141 Z0, Hammerstad–Jensen εeff): L = Z0·√εeff / c and
//               C = √εeff / (Z0·c), so the lossless limit is exactly the line the reflection analysis uses. The
//               dielectric part of C becomes complex with ε(ω): Y = jω C_air · (1 + q (ε(ω) − 1)), q the filling factor
//               (εeff − 1)/(εr − 1) (q = 1 on a stripline).
//
// Not modelled: microstrip dispersion of εeff (Kirschning–Jansen), proximity effect between close conductors, the
// return plane's own DC resistance and anisotropy of the glass weave. No full-wave field solve.
#pragma once

#include <complex>
#include <string>
#include <vector>

#include "sieda/Json.hpp"
#include "sieda/Pcb.hpp"

namespace sieda {

class Project;
using cplx = std::complex<double>;

constexpr double kSpeedOfLight = 299792458.0;            // m/s
constexpr double kVacuumPermeability = 1.25663706212e-6;  // H/m
constexpr double kVacuumPermittivity = 8.8541878128e-12;  // F/m

/// Djordjevic–Sarkar wideband Debye dielectric: ε(ω) = ε∞ + k · log10((ω2 + jω)/(ω1 + jω)), ω1 = 10⁴ and ω2 = 10¹²
/// rad/s, with k and ε∞ chosen so that ε(fRef) = εr (1 − j tan δ). k = 0 (tan δ = 0) is a constant, lossless εr.
struct DielectricModel {
    double erRef = 4.4, tanDRef = 0.02, fRef = 1e9;
    double epsInf = 4.4, k = 0;
    double w1 = 1e4, w2 = 1e12;  // rad/s
    static DielectricModel djordjevicSarkar(double er, double tanD, double fRef = 1e9);
    /// Complex relative permittivity ε' − jε'' at f (Hz).
    cplx epsilon(double f) const;
    double er(double f) const { return epsilon(f).real(); }
    double tanD(double f) const;
};

enum class RoughnessModel { None, Hammerstad, Huray };

/// Copper foil surface: RMS roughness for Hammerstad, sphere radius and surface ratio for Huray.
struct CopperFoil {
    std::string id;    // "smooth", "hvlp", "vlp", "rtf", "std"
    std::string name;
    double rmsUm = 0;  // Hammerstad Δ (µm RMS)
    double radiusUm = 0;      // Huray sphere radius a (µm)
    double surfaceRatio = 0;  // Huray high-frequency excess: K → 1 + surfaceRatio
};
const std::vector<CopperFoil>& copperFoils();
/// The foil by id; "" or unknown picks by laminate: HVLP on low-loss laminates (tan δ < 0.005), standard ED otherwise.
CopperFoil copperFoilFor(const BoardSettings& s, const std::string& id);

/// Skin depth of annealed copper at f (m).
double skinDepth(double f);
/// Surface resistance √(π f μ0 ρ) of copper (Ω per square).
double surfaceResistance(double f);
/// Hammerstad–Jensen roughness factor 1 + (2/π) atan(1.4 (Δ/δ)²), Δ the RMS roughness (m).
double hammerstadRoughness(double f, double rmsM);
/// Huray roughness factor 1 + SR / (1 + δ/a + δ²/(2a²)) with the real skin depth (the textbook form).
double hurayRoughness(double f, double radiusM, double surfaceRatio);
/// The same with the complex skin depth δ(1 − j)/√2: an analytic (causal) function of jω whose real part tracks the
/// textbook factor.
cplx hurayRoughnessCausal(double f, double radiusM, double surfaceRatio);

/// Wheeler's geometry factor R_ac / Rs (1/m) of a stripline between planes `bMm` apart: Pozar eq. 3.198 (both branches,
/// √εr·Z0 below / above 120 Ω), as R = 2·Z0·α_c.
double striplineConductorFactor(double widthMm, double thicknessMm, double bMm, double z0, double er);
/// Wheeler's geometry factor R_ac / Rs (1/m) of a microstrip of width w and thickness t at height h over its plane: the
/// incremental-inductance rule (1/μ0)·∂L/∂n applied numerically to the Hammerstad–Jensen inductance (with their
/// thickness correction). Tends to 2/w for a wide strip (strip underside + plane, Pozar eq. 3.199).
double microstripConductorFactor(double widthMm, double thicknessMm, double heightMm);
/// Hammerstad–Jensen impedance (Ω) of a zero-thickness microstrip in air for u = w/h (accuracy ≈ 0.01 % for u ≤ 1000).
double hammerstadJensenZ0Air(double u);

struct LossOptions {
    std::string foil;  // "" = by laminate
    RoughnessModel roughness = RoughnessModel::Huray;
    bool lossless = false;    // R = G = 0, constant εr: the lossless limit (the reflection analysis' line)
    bool noConductorLoss = false, noDielectricLoss = false;  // for loss break-down
    bool fieldSolver = false;  // Z0, εeff and the R_ac geometry factor from the 2D field solver (cached per geometry)
};
RoughnessModel roughnessFromString(const std::string& s);  // "none", "hammerstad", "huray" (default)

/// Per-unit-length model of one line (SI units: Ω/m, H/m, S/m, F/m).
struct LineModel {
    double z0 = 50;         // lossless (stack-up) impedance, Ω
    double epsEff = 1;      // static effective permittivity at fRef
    double er = 1;          // laminate εr at fRef
    double fill = 1;        // dielectric filling factor q
    bool stripline = false;
    double widthMm = 0.2, thicknessMm = 0.035, heightMm = 0.2;
    DielectricModel dielectric;
    CopperFoil foil;
    RoughnessModel roughness = RoughnessModel::None;
    double rdc = 0;         // Ω/m
    double acFactor = 0;    // R_ac / Rs, 1/m (0 = no conductor loss)
    double lExt = 0;        // external inductance, H/m
    double cAir = 0;        // air capacitance C/εeff, F/m
    static constexpr double kResistanceFloor = 1e-3;  // Ω/m: keeps lossless line matrices regular (negligible loss)

    /// An ideal (lossless, dispersionless) line of impedance z0 and delay per metre `delayPerM` (vias, packages).
    static LineModel ideal(double z0, double delayPerM);
    /// Series impedance R + jωL (internal impedance included) and shunt admittance G + jωC per metre at f.
    cplx seriesZ(double f) const;
    cplx shuntY(double f) const;
    cplx gamma(double f) const;  // propagation constant α + jβ (1/m)
    cplx zc(double f) const;     // characteristic impedance
    /// Attenuation in dB per metre.
    double attenuationDb(double f) const { return 20.0 / std::log(10.0) * gamma(f).real(); }
    /// Internal (skin-effect) impedance per metre, with roughness.
    cplx internalZ(double f) const;
};

/// The line model of a track of `widthMm` on copper layer `layer` of the stack-up.
LineModel lineModel(const BoardSettings& s, int layer, double widthMm, const LossOptions& opt = {});
/// The same with explicit geometry (tests, what-ifs): z0 and εeff of the lossless line, εr / tan δ of the laminate.
LineModel lineModelFrom(bool stripline, double z0, double epsEff, double er, double tanD, double widthMm, double thicknessMm,
                        double heightMm, const CopperFoil& foil, const LossOptions& opt = {});

/// Loss of every copper layer of the stack-up at its single-ended impedance width (or `widthMm` > 0):
/// {"foil","roughness","material","er","tanD","layers":[{layer,name,line,width,z0,dbPerInch:[…],conductorDbPerInch,
/// dielectricDbPerInch, rlgc1GHz:{r,l,g,c}}],"freq":[…]} from 100 MHz to `fMaxHz` (default 20 GHz).
Json lineLossJson(const BoardSettings& s, const LossOptions& opt, double widthMm = 0, double fMaxHz = 20e9);

}  // namespace sieda
