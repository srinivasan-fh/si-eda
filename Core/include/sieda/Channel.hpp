// SiEDA Core — channel analysis of a routed net: the net's copper (track sections as frequency-dependent lossy lines,
// via barrels and stubs as discontinuities, the other pins' loads) becomes a linear network solved per frequency by
// nodal analysis. Its S-parameters at the driver and receiver pads (single-ended 2-port, or a differential pair as a
// 4-port with the P / N sections that run side by side as true coupled lines in even / odd mode) are exported as
// Touchstone, converted to mixed mode (SDD21, SDD11, SCD21) and, terminated by the driver and receiver models,
// give the step response and the eye diagram (Eye.hpp).
//
// Network solution: every element stamps its admittance matrix (lossy line: Y11 = coth(γl)/Zc, Y12 = −csch(γl)/Zc;
// coupled pair: the even- and odd-mode lines combined, Y_PP = (Ye + Yo)/2, Y_PN = (Ye − Yo)/2), every non-port node is
// eliminated (Kron reduction, minimum-degree order computed once) and the port admittance matrix converts to
// S = (I + Z0·Y)⁻¹ (I − Z0·Y). Imported Touchstone blocks cascade with the Redheffer star product.
#pragma once

#include <complex>
#include <string>
#include <vector>

#include "sieda/Ibis.hpp"
#include "sieda/Json.hpp"
#include "sieda/LossyLine.hpp"

namespace sieda {

class Project;

/// Small dense complex matrix (row-major).
struct CMat {
    int n = 0;
    std::vector<cplx> a;
    CMat() = default;
    explicit CMat(int size) : n(size), a(static_cast<size_t>(size * size), cplx(0, 0)) {}
    cplx& operator()(int i, int j) { return a[static_cast<size_t>(i * n + j)]; }
    const cplx& operator()(int i, int j) const { return a[static_cast<size_t>(i * n + j)]; }
    static CMat identity(int size);
};
CMat operator*(const CMat& x, const CMat& y);
CMat operator+(const CMat& x, const CMat& y);
CMat operator-(const CMat& x, const CMat& y);
/// Gauss–Jordan inverse with partial pivoting; false when singular.
bool invertInPlace(CMat& m);
/// Sub-block rows [r0, r0+rows), cols [c0, c0+cols) (square blocks only: rows == cols).
CMat block(const CMat& m, int r0, int c0, int size);

/// A linear passive network: lossy lines, coupled line pairs and shunts to ground.
struct ChannelNetwork {
    struct Line {
        int a = 0, b = 0;
        LineModel model;
        double length = 0;  // m
    };
    /// Two coupled lines a1→b1 and a2→b2 (a2 beside a1) in even / odd mode.
    struct Coupled {
        int a1 = 0, b1 = 0, a2 = 0, b2 = 0;
        LineModel even, odd;
        double length = 0;  // m
    };
    struct Shunt {
        int node = 0;
        double g = 0, c = 0;  // S, F to ground
        double j = 0;         // A injected (G·V_rail of a termination), DC solve only
    };
    int nodes = 0;
    std::vector<Line> lines;
    std::vector<Coupled> coupled;
    std::vector<Shunt> shunts;
    std::vector<int> ports;
    int addNode() { return nodes++; }
    /// Port admittance matrices (ports × ports) at each frequency (Hz, ≥ 0).
    std::vector<CMat> portAdmittance(const std::vector<double>& freq) const;
    /// S-parameters referenced to z0 (Ω, real) at each frequency.
    std::vector<CMat> sParameters(const std::vector<double>& freq, double z0) const;
    /// DC voltages at `observe` with the shunts' injections plus `extra` Norton elements.
    std::vector<double> dcVoltages(const std::vector<Shunt>& extra, const std::vector<int>& observe) const;
};

/// Two-port Y entries (Y11 = Y22, Y12 = Y21) of a line of `lengthM` at f.
std::pair<cplx, cplx> lineAdmittance(const LineModel& m, double lengthM, double f);

/// S-parameters on a frequency grid.
struct SParams {
    int ports = 0;
    double z0 = 50;
    std::vector<double> freq;
    std::vector<CMat> s;
};

/// Mixed-mode S of a 4-port ordered [P near, N near, P far, N far]: rows / cols [D1, D2, C1, C2] (Sdd, Sdc / Scd, Scc).
CMat mixedMode(const CMat& s4);
/// Redheffer star product of two 2N-ports ordered [N near ports, N far ports]: `a`'s far side joins `b`'s near side.
CMat cascadeStar(const CMat& a, const CMat& b);
/// `src` resampled at `freq`: linear in magnitude and unwrapped phase; outside its band the magnitude is held and the
/// phase extrapolated linearly (constant group delay).
SParams interpolateSParams(const SParams& src, const std::vector<double>& freq);

/// A port termination in wave form: a = Γ·b + c (Γ reflection of the termination, c the incident source wave).
struct PortTermination {
    cplx gamma = 0, c = 0;
    static PortTermination source(cplx vth, cplx zth, double z0);  // Thévenin source
    static PortTermination load(cplx y, double z0);                // passive admittance (0 = open)
};
/// Port voltages of an N-port S (ref z0) with every port terminated.
std::vector<cplx> terminatedVoltages(const CMat& s, double z0, const std::vector<PortTermination>& t);

/// Radix-2 FFT in place (size a power of two); inverse = true applies e^{+j…} and the 1/N factor.
void fft(std::vector<cplx>& x, bool inverse);

// ---- routed-net channel -------------------------------------------------------------------------------------------------

struct ChannelOptions {
    std::string net;
    std::string partner;   // "" = the differential-pair partner by name; "none" = single-ended
    std::string receiver;  // receiver reference ("U2"); "" = the farthest receiver
    double fMax = 20e9;
    int points = 401;
    double refOhms = 50;   // single-ended port reference (differential 2×, common ½×)
    LossOptions loss;
};

struct ChannelModel {
    std::string error;
    std::string netP, netN;
    bool differential = false, estimated = false;
    ChannelNetwork network;  // ports: [driver, receiver] or [P driver, N driver, P receiver, N receiver]
    DriverModel driver, receiver;
    double seriesR = 0;  // series termination found in the schematic
    std::string driverRef, driverPin, receiverRef, receiverPin;
    double lengthP = 0, lengthN = 0, delayP = 0, delayN = 0;  // driver → receiver path (mm, s)
    int vias = 0;
    struct CoupledSection {
        std::string layer;
        double length = 0, gap = 0;  // mm
        double zEven = 0, zOdd = 0, zDiff = 0, zComm = 0, epsEven = 0, epsOdd = 0;
    };
    std::vector<CoupledSection> coupled;
    double coupledLength = 0;
    std::vector<std::string> notes;
};
ChannelModel extractChannel(const Project& project, const ChannelOptions& opt);

struct TouchstoneData;
struct EyeOptions;

/// How the channel is driven for the step response and the eye.
struct ChannelDrive {
    bool idealDriver = false;  // true: ideal source / termination of the reference impedance (per leg)
    double swing = 1.0;        // ideal: V per leg
    double riseTime = 0;       // 10–90 % (s); 0 = the driver model's (ideal: 0.25 UI or 50 ps)
    double sourceOhms = 50, termOhms = 50;
};

/// S-parameters of the channel (with `cascade` appended at its far end) on `freq`.
SParams channelSParams(const ChannelModel& m, const std::vector<double>& freq, double z0, const TouchstoneData* cascade = nullptr);
/// Frequency response from the driver's source (one leg, unit volts; differential +1 V on P and −1 V on N) to the
/// receiver voltage (differential V_P − V_N), with the terminations of `drive`, at each frequency.
std::vector<cplx> channelTransfer(const ChannelModel& m, const std::vector<double>& freq, double z0, const ChannelDrive& drive,
                                  const TouchstoneData* cascade = nullptr);
/// DC receiver voltage (differential V_P − V_N) with the driver's source(s) at `vP` (P leg) and `vN` (N leg).
double channelDcLevel(const ChannelModel& m, double vP, double vN, const ChannelDrive& drive);

ChannelOptions channelOptionsFromJson(const Json& j);
ChannelDrive channelDriveFromJson(const Json& j);

/// Full analysis: {"net","partner","differential","ports","refOhms","driver":{…},"receiver":{…},"length","skew",
/// "coupled":[…],"freq":[…],"curves":[{name,db:[…]}],"nyquist":{f,il,rl},"step":{time,lossy,lossless},
/// "eye":{…} (Eye.hpp),"notes":[…]} or {"error"}.
Json channelJson(const Project& project, const ChannelOptions& opt, const ChannelDrive& drive, const EyeOptions* eye,
                 const TouchstoneData* cascade);
/// The channel as Touchstone 1.1 text (.s2p / .s4p, RI, Hz, reference refOhms); "" with `error` set on failure.
std::string channelTouchstone(const Project& project, const ChannelOptions& opt, std::string* error);

}  // namespace sieda
