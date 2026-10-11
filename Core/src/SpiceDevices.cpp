#include "SpiceDevices.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace sieda::spicedev {

namespace {
double pnjlim(double vnew, double vold, double vt, double vcrit) {
    if (vnew > vcrit && std::fabs(vnew - vold) > 2 * vt) {
        if (vold > 0) {
            const double arg = 1 + (vnew - vold) / vt;
            return arg > 0 ? vold + vt * std::log(arg) : vcrit;
        }
        return vt * std::log(std::max(vnew / vt, 1e-300));
    }
    return vnew;
}
constexpr double kInf = std::numeric_limits<double>::infinity();
constexpr double kEpsOx = 3.453e-11;  // F/m, SiO2

double param(const SpicePrimitive& p, std::initializer_list<const char*> names, double def) {
    for (const char* n : names) {
        auto it = p.model.find(n);
        if (it != p.model.end()) return it->second;
    }
    return def;
}

/// is·(exp(v / nvt) − 1), continued linearly above the knee where the current would exceed 10 kA (Newton steps
/// cannot overflow).
double junction(double is, double v, double nvt) {
    if (!(is > 0)) return 0.0;
    const double x = v / nvt;
    const double knee = std::min(700.0, std::max(40.0, std::log(1e4 / is)));
    const double e = x < knee ? std::exp(x) : std::exp(knee) * (1.0 + x - knee);
    return is * (e - 1.0);
}

double safeInv(double v) { return v > 0 && std::isfinite(v) ? 1.0 / v : 0.0; }

double kelvin(double celsius) { return celsius == 27 ? kNominalTemp : celsius + 273.15; }

/// SPICE3 temperature updates (diotemp.c, bjttemp.c, mostemp.c, jfettemp.c) of a model given at TNOM and simulated
/// at T: the thermal voltage, the junction potentials and capacitances, the saturation currents. At T == TNOM every
/// value is returned untouched, so nominal results stay bit-identical.
struct Temp {
    double t, tnom, vt, ratio;
    bool nominal;
    double fact1, fact2, pbfact, pbfact1, egfet, egfet1;
    Temp(double tempC, double tnomC) {
        if (std::fabs(tempC - tnomC) < 1e-9) tempC = tnomC;
        t = kelvin(tempC);
        tnom = kelvin(tnomC);
        nominal = t == tnom;
        vt = t == kNominalTemp ? kVtNominal : kBoltzmann * t / kCharge;
        ratio = t / tnom;
        auto bandGap = [](double tk) { return 1.16 - 7.02e-4 * tk * tk / (tk + 1108); };
        auto pb = [](double tk, double eg) {
            const double arg = -eg / (2 * kBoltzmann * tk) + 1.1150877 / (2 * kBoltzmann * kNominalTemp);
            return -2 * (kBoltzmann * tk / kCharge) * (1.5 * std::log(tk / kNominalTemp) + kCharge * arg);
        };
        egfet = bandGap(t);
        egfet1 = bandGap(tnom);
        fact1 = tnom / kNominalTemp;
        fact2 = t / kNominalTemp;
        pbfact = pb(t, egfet);
        pbfact1 = pb(tnom, egfet1);
    }
    double potential(double vj) const { return nominal ? vj : pbfact + fact2 * (vj - pbfact1) / fact1; }
    double capacitance(double cj0, double vj, double m) const {
        if (nominal) return cj0;
        const double pbo = (vj - pbfact1) / fact1, gmaold = (vj - pbo) / pbo, gmanew = (potential(vj) - pbo) / pbo;
        return cj0 * (1 + m * (4e-4 * (t - kNominalTemp) - gmanew)) / (1 + m * (4e-4 * (tnom - kNominalTemp) - gmaold));
    }
    /// IS · (T/TNOM)^(XTI/N) · exp(EG / (N·V_T) · (T/TNOM − 1)).
    double satCur(double is, double eg, double xti, double n) const {
        return nominal ? is : is * std::exp((ratio - 1) * eg / (n * vt) + xti / n * std::log(ratio));
    }
    double betaFactor(double xtb) const { return nominal ? 1.0 : std::exp(std::log(ratio) * xtb); }
};
Temp modelTemp(const SpicePrimitive& p, double tempC) { return Temp(tempC, param(p, {"TNOM", "T_MEASURED"}, p.tnom)); }

// ------------------------------------------------------------------ diode

class Diode final : public Device {
public:
    Diode(const SpicePrimitive& p, double tempC) {
        const Temp tp = modelTemp(p, tempC);
        const double area = p.area * p.multiplier;
        vt_ = tp.vt;
        n_ = std::max(param(p, {"N"}, 1.0), 0.05);
        is_ = tp.satCur(param(p, {"IS"}, 1e-14), param(p, {"EG"}, 1.11), param(p, {"XTI"}, 3.0), n_) * area;
        const double vj = std::max(param(p, {"VJ", "PB"}, 1.0), 0.05);
        m_ = std::clamp(param(p, {"M", "MJ"}, 0.5), 0.0, 0.99);
        cj0_ = tp.capacitance(param(p, {"CJO", "CJ0", "CJ"}, 0.0), vj, m_) * area;
        vj_ = tp.potential(vj);
        fc_ = std::clamp(param(p, {"FC"}, 0.5), 0.0, 0.95);
        tt_ = std::max(param(p, {"TT"}, 0.0), 0.0);
        bv_ = param(p, {"BV", "VB"}, kInf);
        if (!(bv_ > 0)) bv_ = kInf;
        ibv_ = std::max(param(p, {"IBV"}, 1e-3), 1e-30) * area;
        nbv_ = std::max(param(p, {"NBV"}, n_), 0.05);
        kf_ = param(p, {"KF"}, 0.0);
        af_ = param(p, {"AF"}, 1.0);
        double rs = param(p, {"RS"}, 0.0);
        seriesR[0] = rs > 0 ? rs / area : 0.0;
        terminals = 2;
    }
    double forward(double v) const { return junction(is_, v, n_ * vt_); }
    double total(double v) const {
        double i = forward(v);
        if (std::isfinite(bv_)) {
            // Reverse breakdown: −IBV·exp(−(v + BV) / (NBV·Vt)), negligible above −BV.
            i -= junction(ibv_, -(v + bv_), nbv_ * vt_) + ibv_;
        }
        return i;
    }
    void currents(const double* v, double* i) const override {
        double id = total(v[0] - v[1]);
        i[0] = id;
        i[1] = -id;
    }
    bool hasCharge() const override { return cj0_ > 0 || tt_ > 0; }
    void charges(const double* v, double* q) const override {
        const double vd = v[0] - v[1];
        double qd = depletionCharge(cj0_, vj_, m_, fc_, vd) + tt_ * forward(vd);
        q[0] = qd;
        q[1] = -qd;
    }
    void limit(double* v, std::array<double, 2>& old, bool& valid) const override {
        const double nvt = n_ * vt_;
        double vd = v[0] - v[1];
        if (valid) vd = pnjlim(vd, old[0], nvt, vcrit(nvt));
        old[0] = vd;
        valid = true;
        v[0] = v[1] + vd;
    }
    double vcrit(double nvt) const { return nvt * std::log(nvt / (1.4142135623730951 * std::max(is_, 1e-300))); }
    void noise(const double* v, std::vector<NoiseTerm>& out) const override {
        double id = std::fabs(total(v[0] - v[1]));
        out.push_back({0, 1, 2 * kCharge * id, kf_ * std::pow(id, af_), "shot"});
    }

private:
    double vt_, is_, n_, cj0_, vj_, m_, fc_, tt_, bv_, ibv_, nbv_, kf_, af_;
};

// ------------------------------------------------------------------ BJT (Gummel–Poon)

class Bjt final : public Device {
public:
    Bjt(const SpicePrimitive& p, double tempC) {
        const Temp tp = modelTemp(p, tempC);
        sign_ = p.modelType == "PNP" ? -1.0 : 1.0;
        const double area = p.area * p.multiplier;
        vt_ = tp.vt;
        // IS, ISE, ISC with XTI / EG; BF, BR with XTB (the leakage currents divided by the same factor, as SPICE).
        const double eg = param(p, {"EG"}, 1.11), xti = param(p, {"XTI"}, 3.0);
        const double bfactor = tp.betaFactor(param(p, {"XTB"}, 0.0));
        is_ = tp.satCur(param(p, {"IS"}, 1e-16), eg, xti, 1.0) * area;
        bf_ = std::max(param(p, {"BF"}, 100.0) * bfactor, 1e-6);
        br_ = std::max(param(p, {"BR"}, 1.0) * bfactor, 1e-6);
        nf_ = std::max(param(p, {"NF"}, 1.0), 0.05);
        nr_ = std::max(param(p, {"NR"}, 1.0), 0.05);
        invVaf_ = safeInv(param(p, {"VAF", "VA"}, kInf));
        invVar_ = safeInv(param(p, {"VAR", "VB"}, kInf));
        invIkf_ = safeInv(param(p, {"IKF", "IK"}, kInf) * area);
        invIkr_ = safeInv(param(p, {"IKR"}, kInf) * area);
        ne_ = std::max(param(p, {"NE"}, 1.5), 0.05);
        nc_ = std::max(param(p, {"NC"}, 2.0), 0.05);
        ise_ = tp.satCur(param(p, {"ISE"}, 0.0), eg, xti, ne_) / bfactor * area;
        isc_ = tp.satCur(param(p, {"ISC"}, 0.0), eg, xti, nc_) / bfactor * area;
        const double vje = std::max(param(p, {"VJE", "PE"}, 0.75), 0.05), vjc = std::max(param(p, {"VJC", "PC"}, 0.75), 0.05);
        mje_ = std::clamp(param(p, {"MJE", "ME"}, 0.33), 0.0, 0.99);
        mjc_ = std::clamp(param(p, {"MJC", "MC"}, 0.33), 0.0, 0.99);
        cje_ = tp.capacitance(param(p, {"CJE"}, 0.0), vje, mje_) * area;
        cjc_ = tp.capacitance(param(p, {"CJC"}, 0.0), vjc, mjc_) * area;
        vje_ = tp.potential(vje);
        vjc_ = tp.potential(vjc);
        tf_ = std::max(param(p, {"TF"}, 0.0), 0.0);
        tr_ = std::max(param(p, {"TR"}, 0.0), 0.0);
        fc_ = std::clamp(param(p, {"FC"}, 0.5), 0.0, 0.95);
        kf_ = param(p, {"KF"}, 0.0);
        af_ = param(p, {"AF"}, 1.0);
        auto r = [&](const char* k) {
            double v = param(p, {k}, 0.0);
            return v > 0 ? v / area : 0.0;
        };
        seriesR = {r("RC"), r("RB"), r("RE"), 0.0};  // terminals C, B, E
        terminals = 3;
    }
    struct Op {
        double cbe, cbc, qb, ic, ib;
    };
    Op op(double vbe, double vbc) const {
        const double vt = vt_;
        Op o{};
        o.cbe = junction(is_, vbe, nf_ * vt);
        o.cbc = junction(is_, vbc, nr_ * vt);
        const double cben = junction(ise_, vbe, ne_ * vt);
        const double cbcn = junction(isc_, vbc, nc_ * vt);
        double q1d = 1.0 - vbc * invVaf_ - vbe * invVar_;
        q1d = std::max(q1d, 1e-3);
        const double q1 = 1.0 / q1d;
        const double q2 = o.cbe * invIkf_ + o.cbc * invIkr_;
        o.qb = q2 != 0 ? q1 * (1.0 + std::sqrt(std::max(0.0, 1.0 + 4.0 * q2))) / 2.0 : q1;
        o.ic = (o.cbe - o.cbc) / o.qb - o.cbc / br_ - cbcn;
        o.ib = o.cbe / bf_ + cben + o.cbc / br_ + cbcn;
        return o;
    }
    void currents(const double* v, double* i) const override {  // C, B, E
        const double vbe = sign_ * (v[1] - v[2]), vbc = sign_ * (v[1] - v[0]);
        Op o = op(vbe, vbc);
        i[0] = sign_ * o.ic;
        i[1] = sign_ * o.ib;
        i[2] = -(i[0] + i[1]);
    }
    bool hasCharge() const override { return cje_ > 0 || cjc_ > 0 || tf_ > 0 || tr_ > 0; }
    void charges(const double* v, double* q) const override {
        const double vbe = sign_ * (v[1] - v[2]), vbc = sign_ * (v[1] - v[0]);
        Op o = op(vbe, vbc);
        const double qbe = tf_ * o.cbe / o.qb + depletionCharge(cje_, vje_, mje_, fc_, vbe);
        const double qbc = tr_ * o.cbc + depletionCharge(cjc_, vjc_, mjc_, fc_, vbc);
        q[0] = -sign_ * qbc;
        q[1] = sign_ * (qbe + qbc);
        q[2] = -sign_ * qbe;
    }
    void limit(double* v, std::array<double, 2>& old, bool& valid) const override {  // C, B, E
        double vbe = sign_ * (v[1] - v[2]), vbc = sign_ * (v[1] - v[0]);
        if (valid) {
            vbe = pnjlim(vbe, old[0], nf_ * vt_, crit(nf_ * vt_));
            vbc = pnjlim(vbc, old[1], nr_ * vt_, crit(nr_ * vt_));
        }
        old = {vbe, vbc};
        valid = true;
        v[2] = v[1] - sign_ * vbe;
        v[0] = v[1] - sign_ * vbc;
    }
    double crit(double vt) const { return vt * std::log(vt / (1.4142135623730951 * std::max(is_, 1e-300))); }
    void noise(const double* v, std::vector<NoiseTerm>& out) const override {
        const double vbe = sign_ * (v[1] - v[2]), vbc = sign_ * (v[1] - v[0]);
        Op o = op(vbe, vbc);
        const double ic = std::fabs(o.ic), ib = std::fabs(o.ib);
        out.push_back({0, 2, 2 * kCharge * ic, 0.0, "shot"});
        out.push_back({1, 2, 2 * kCharge * ib, kf_ * std::pow(ib, af_), "shot"});
    }

private:
    double sign_, vt_, is_, bf_, br_, nf_, nr_, invVaf_, invVar_, invIkf_, invIkr_, ise_, ne_, isc_, nc_;
    double cje_, vje_, mje_, cjc_, vjc_, mjc_, tf_, tr_, fc_, kf_, af_;
};

// ------------------------------------------------------------------ MOSFET (levels 1 and 3)

constexpr double kEpsSi = 11.7 * 8.854214871e-12;  // F/m (SPICE3 EPSSIL)

class Mosfet final : public Device {
public:
    Mosfet(const SpicePrimitive& p, double tempC) {
        const Temp tp = modelTemp(p, tempC);
        sign_ = p.modelType == "PMOS" ? -1.0 : 1.0;
        const double m = p.multiplier;
        const double ld = std::max(param(p, {"LD"}, 0.0), 0.0);
        leff_ = std::max(p.l - 2 * ld, p.l * 1e-3);
        w_ = p.w * m;
        vt_ = tp.vt;
        tox_ = param(p, {"TOX"}, 0.0);
        cox_ = tox_ > 0 ? kEpsOx / tox_ : 0.0;
        // KP and UO fall as (T/TNOM)^-1.5 (mostemp.c).
        const double ratio4 = tp.nominal ? 1.0 : tp.ratio * std::sqrt(tp.ratio);
        const double uo = param(p, {"UO", "U0"}, 0.0);
        double kp = param(p, {"KP"}, 0.0);
        if (!(kp > 0)) kp = uo > 0 && cox_ > 0 ? uo * 1e-4 * cox_ : 2e-5;
        if (!tp.nominal) kp /= ratio4;
        beta_ = kp * w_ / leff_;
        uo_ = (uo > 0 ? uo : 600.0) / ratio4;
        gamma_ = std::max(param(p, {"GAMMA"}, 0.0), 0.0);
        phi_ = std::max(param(p, {"PHI"}, 0.6), 0.1);
        vto_ = sign_ * param(p, {"VTO", "VT0"}, 0.0);
        if (!tp.nominal) {
            // VTO and PHI at T: the flat-band voltage follows the band gap and the Fermi potential (mostemp.c).
            const double tPhi = tp.potential(phi_);
            const double vbi = vto_ - gamma_ * std::sqrt(phi_) + sign_ * 0.5 * (tp.egfet1 - tp.egfet) + 0.5 * (tPhi - phi_);
            phi_ = tPhi;
            vto_ = vbi + gamma_ * std::sqrt(phi_);
        }
        level3_ = p.level == 3;
        lambda_ = level3_ ? 0.0 : param(p, {"LAMBDA"}, 0.0);
        theta_ = level3_ ? std::max(param(p, {"THETA"}, 0.0), 0.0) : 0.0;
        if (level3_) {
            // SPICE3 MOS3 constants (mos3temp.c): the oxide defaults to 100 nm, NSUB gives the depletion-layer width.
            const double cox = cox_ > 0 ? cox_ : kEpsOx / 1e-7;
            const double nsub = param(p, {"NSUB"}, 0.0) * 1e6;
            alpha_ = nsub > 0 ? (kEpsSi + kEpsSi) / (kCharge * nsub) : 0.0;
            xd_ = std::sqrt(alpha_);
            xj_ = std::max(param(p, {"XJ"}, 0.0), 0.0);
            ld_ = ld;
            eta_ = param(p, {"ETA"}, 0.0) * 8.15e-22 / (cox * leff_ * leff_ * leff_);
            narrow_ = param(p, {"DELTA"}, 0.0) * 0.5 * 3.14159265358979323846 * kEpsSi / cox / std::max(p.w, 1e-12);
            csonco_ = std::max(param(p, {"NFS"}, 0.0), 0.0) * 1e4 * kCharge / cox;
            vmax_ = std::max(param(p, {"VMAX"}, 0.0), 0.0);
            kappa_ = param(p, {"KAPPA"}, 0.2);
            vbi_ = vto_ - gamma_ * std::sqrt(phi_);
        }
        is_ = param(p, {"IS"}, 1e-14) * m;
        if (!tp.nominal) is_ *= std::exp(-tp.egfet / tp.vt + tp.egfet1 / (kBoltzmann * tp.tnom / kCharge));
        const double pb = std::max(param(p, {"PB"}, 0.8), 0.05);
        mj_ = std::clamp(param(p, {"MJ"}, 0.5), 0.0, 0.99);
        mjsw_ = std::clamp(param(p, {"MJSW"}, 0.33), 0.0, 0.99);
        fc_ = std::clamp(param(p, {"FC"}, 0.5), 0.0, 0.95);
        const double cj = tp.capacitance(param(p, {"CJ"}, 0.0), pb, mj_), cjsw = tp.capacitance(param(p, {"CJSW"}, 0.0), pb, mjsw_);
        cbd_ = tp.capacitance(param(p, {"CBD"}, 0.0), pb, mj_) * m;
        cbs_ = tp.capacitance(param(p, {"CBS"}, 0.0), pb, mj_) * m;
        pb_ = tp.potential(pb);
        cbdArea_ = (cj * p.ad) * m;
        cbsArea_ = (cj * p.as) * m;
        cbdSw_ = cjsw * p.pd * m;
        cbsSw_ = cjsw * p.ps * m;
        cgs_ = param(p, {"CGSO"}, 0.0) * w_;
        cgd_ = param(p, {"CGDO"}, 0.0) * w_;
        cgb_ = param(p, {"CGBO"}, 0.0) * leff_ * m;
        cint_ = cox_ * w_ * leff_;
        kf_ = param(p, {"KF"}, 0.0);
        af_ = param(p, {"AF"}, 1.0);
        auto r = [&](const char* k) {
            double v = param(p, {k}, 0.0);
            return v > 0 ? v / m : 0.0;
        };
        seriesR = {r("RD"), 0.0, r("RS"), 0.0};  // D, G, S, B
        terminals = 4;
    }
    double threshold(double vbs) const {
        if (gamma_ == 0) return vto_;
        const double s = vbs <= 0 ? std::sqrt(phi_ - vbs) : std::sqrt(phi_) / (1.0 + 0.5 * vbs / phi_);
        return vto_ + gamma_ * (s - std::sqrt(phi_));
    }
    /// Drain current (s-space, drain → source) for vds ≥ 0.
    double channel(double vgs, double vds, double vbs) const {
        if (level3_) return channel3(vgs, vds, vbs);
        const double vgst = vgs - threshold(vbs);
        if (vgst <= 0) return 0.0;
        if (vds < vgst) return beta_ * (vgst - 0.5 * vds) * vds * (1.0 + lambda_ * vds);
        return 0.5 * beta_ * vgst * vgst * (1.0 + lambda_ * vds);
    }
    /// SPICE3 level 3 (mos3load.c): short-channel and narrow-width threshold (XJ / NSUB, DELTA), ETA static feedback,
    /// NFS weak inversion, THETA mobility reduction, VMAX velocity saturation, KAPPA channel-length modulation.
    double channel3(double vgs, double vds, double vbs) const {
        double phibs, sqphbs;
        if (vbs <= 0) {
            phibs = phi_ - vbs;
            sqphbs = std::sqrt(phibs);
        } else {
            sqphbs = std::sqrt(phi_) / (1.0 + vbs / (phi_ + phi_));
            phibs = sqphbs * sqphbs;
        }
        double fshort = 1.0;
        if (xj_ > 0 && xd_ > 0) {  // charge shared with the junction depletion layers
            const double wps = xd_ * sqphbs, xjonxl = xj_ / leff_, djonxj = ld_ / xj_, wponxj = wps / xj_;
            const double wconxj = 0.0631353 + 0.8013292 * wponxj - 0.01110777 * wponxj * wponxj;
            const double arga = wconxj + djonxj, argc = wponxj / (1.0 + wponxj), argb = std::sqrt(1.0 - argc * argc);
            fshort = 1.0 - xjonxl * (arga * argb - djonxj);
        }
        const double gammas = gamma_ * fshort;
        const double fbody = 0.5 * gammas / (sqphbs + sqphbs) + narrow_;
        const double qbonco = gammas * sqphbs + narrow_ * phibs;
        const double vth = vbi_ - eta_ * vds + qbonco;
        double von = vth, xn = 1.0;
        if (csonco_ > 0) {  // joint weak and strong inversion
            xn = 1.0 + csonco_ + qbonco / (phibs + phibs);
            von = vth + vt_ * xn;
        } else if (vgs <= von) {
            return 0.0;
        }
        const double vgsx = std::max(vgs, von);
        const double fgate = 1.0 / (1.0 + theta_ * (vgsx - vth));
        double vdsat = (vgsx - vth) / (1.0 + fbody), onvdsc = 0.0;
        if (vmax_ > 0) {
            const double vdsc = leff_ * vmax_ / (uo_ * 1e-4 * fgate);
            onvdsc = 1.0 / vdsc;
            vdsat = vdsat + vdsc - std::sqrt(vdsat * vdsat + vdsc * vdsc);
        }
        const double vdsx = std::min(vds, vdsat);
        if (vdsx == 0.0) return 0.0;
        double cdrain = beta_ * fgate * (vgsx - vth - 0.5 * (1.0 + fbody) * vdsx) * vdsx;
        double fdrain = 1.0;
        if (vmax_ > 0) {
            fdrain = 1.0 / (1.0 + vdsx * onvdsc);
            cdrain *= fdrain;
        }
        if (vds > vdsat && alpha_ > 0) {  // channel length modulation
            double delxl;
            if (vmax_ > 0) {
                const double gdsat = std::max(1e-12, cdrain * (1.0 - fdrain) * onvdsc);
                const double emax = kappa_ * cdrain / (leff_ * gdsat);
                const double arga = 0.5 * emax * alpha_, argc = kappa_ * alpha_;
                delxl = std::sqrt(arga * arga + argc * (vds - vdsat)) - arga;
            } else {
                delxl = std::sqrt(kappa_ * (vds - vdsat) * alpha_);
            }
            if (delxl > 0.5 * leff_) delxl = leff_ - leff_ * leff_ / (4.0 * delxl);  // punch-through approximation
            cdrain /= 1.0 - delxl / leff_;
        }
        if (vgs < von) cdrain *= std::exp((vgs - von) / (xn * vt_));
        return cdrain;
    }
    struct Bias {
        double vgs, vds, vbs, vbd, vgd, vgb;
    };
    Bias bias(const double* v) const {
        Bias b{};
        b.vgs = sign_ * (v[1] - v[2]);
        b.vds = sign_ * (v[0] - v[2]);
        b.vbs = sign_ * (v[3] - v[2]);
        b.vbd = sign_ * (v[3] - v[0]);
        b.vgd = sign_ * (v[1] - v[0]);
        b.vgb = sign_ * (v[1] - v[3]);
        return b;
    }
    double ids(const Bias& b) const {  // drain → source, s-space, either mode
        if (b.vds >= 0) return channel(b.vgs, b.vds, b.vbs);
        return -channel(b.vgd, -b.vds, b.vbd);
    }
    void currents(const double* v, double* i) const override {  // D, G, S, B
        const Bias b = bias(v);
        const double id = ids(b);
        const double ibd = junction(is_, b.vbd, vt_), ibs = junction(is_, b.vbs, vt_);
        i[0] = sign_ * (id - ibd);
        i[1] = 0.0;
        i[2] = sign_ * (-id - ibs);
        i[3] = sign_ * (ibd + ibs);
    }
    bool hasCharge() const override {
        return cbd_ > 0 || cbs_ > 0 || cbdArea_ > 0 || cbsArea_ > 0 || cbdSw_ > 0 || cbsSw_ > 0 || cgs_ > 0 || cgd_ > 0 ||
               cgb_ > 0 || cint_ > 0;
    }
    void charges(const double* v, double* q) const override {
        const Bias b = bias(v);
        q[0] = q[1] = q[2] = q[3] = 0.0;
        auto pair = [&](int x, int y, double charge) {
            q[x] += sign_ * charge;
            q[y] -= sign_ * charge;
        };
        pair(1, 2, cgs_ * b.vgs);
        pair(1, 0, cgd_ * b.vgd);
        pair(1, 3, cgb_ * b.vgb);
        pair(3, 0, depletionCharge(cbd_ + cbdArea_, pb_, mj_, fc_, b.vbd) + depletionCharge(cbdSw_, pb_, mjsw_, fc_, b.vbd));
        pair(3, 2, depletionCharge(cbs_ + cbsArea_, pb_, mj_, fc_, b.vbs) + depletionCharge(cbsSw_, pb_, mjsw_, fc_, b.vbs));
        if (cint_ > 0) {
            // Intrinsic gate charge in inversion: 2/3·Cox·W·L towards the source end (saturation value), turned on
            // smoothly over ~50 mV around the threshold.
            const bool forward = b.vds >= 0;
            const double vg = forward ? b.vgs : b.vgd;
            const double vgst = vg - threshold(forward ? b.vbs : b.vbd);
            const double w = 0.05;
            const double soft = vgst / w > 30 ? vgst : w * std::log1p(std::exp(vgst / w));
            pair(1, forward ? 2 : 0, (2.0 / 3.0) * cint_ * soft);
        }
    }
    void noise(const double* v, std::vector<NoiseTerm>& out) const override {
        const Bias b = bias(v);
        const double h = 1e-6;
        Bias up = b, dn = b;
        up.vgs += h;
        dn.vgs -= h;
        up.vgd += h;
        dn.vgd -= h;
        const double gm = std::fabs(ids(up) - ids(dn)) / (2 * h);
        const double id = std::fabs(ids(b));
        double flick = kf_ * std::pow(id, af_);
        if (cox_ > 0) flick /= cox_ * leff_ * leff_;
        out.push_back({0, 2, 8.0 / 3.0 * kBoltzmann * kNominalTemp * gm, flick, "thermal"});
    }

private:
    double sign_, leff_, w_, vt_, tox_, cox_, beta_, uo_, vto_, gamma_, phi_, lambda_, theta_, is_, pb_, mj_, mjsw_, fc_;
    double cbd_, cbs_, cbdArea_, cbsArea_, cbdSw_, cbsSw_, cgs_, cgd_, cgb_, cint_, kf_, af_;
    bool level3_ = false;
    double alpha_ = 0, xd_ = 0, xj_ = 0, ld_ = 0, eta_ = 0, narrow_ = 0, csonco_ = 0, vmax_ = 0, kappa_ = 0, vbi_ = 0;
};

// ------------------------------------------------------------------ JFET

class Jfet final : public Device {
public:
    Jfet(const SpicePrimitive& p, double tempC) {
        const Temp tp = modelTemp(p, tempC);
        sign_ = p.modelType == "PJF" ? -1.0 : 1.0;
        const double area = p.area * p.multiplier;
        vt_ = tp.vt;
        // IS with EG = 1.11 (jfettemp.c); VTO and BETA with the PSpice coefficients VTOTC (V/°C) and BETATCE (%/°C).
        const double dt = tp.nominal ? 0.0 : tp.t - tp.tnom;
        vto_ = param(p, {"VTO", "VT0"}, -2.0) + param(p, {"VTOTC"}, 0.0) * dt;
        beta_ = param(p, {"BETA"}, 1e-4) * area * (tp.nominal ? 1.0 : std::pow(1.01, param(p, {"BETATCE"}, 0.0) * dt));
        lambda_ = param(p, {"LAMBDA"}, 0.0);
        n_ = std::max(param(p, {"N"}, 1.0), 0.05);
        is_ = tp.satCur(param(p, {"IS"}, 1e-14), 1.11, 0.0, 1.0) * area;
        const double pb = std::max(param(p, {"PB"}, 1.0), 0.05);
        m_ = std::clamp(param(p, {"M"}, 0.5), 0.0, 0.99);
        cgs_ = tp.capacitance(param(p, {"CGS"}, 0.0), pb, m_) * area;
        cgd_ = tp.capacitance(param(p, {"CGD"}, 0.0), pb, m_) * area;
        pb_ = tp.potential(pb);
        fc_ = std::clamp(param(p, {"FC"}, 0.5), 0.0, 0.95);
        kf_ = param(p, {"KF"}, 0.0);
        af_ = param(p, {"AF"}, 1.0);
        auto r = [&](const char* k) {
            double v = param(p, {k}, 0.0);
            return v > 0 ? v / area : 0.0;
        };
        seriesR = {r("RD"), 0.0, r("RS"), 0.0};  // D, G, S
        terminals = 3;
    }
    double channel(double vgs, double vds) const {
        const double vgst = vgs - vto_;
        if (vgst <= 0) return 0.0;
        if (vds < vgst) return beta_ * vds * (2 * vgst - vds) * (1 + lambda_ * vds);
        return beta_ * vgst * vgst * (1 + lambda_ * vds);
    }
    double ids(double vgs, double vgd, double vds) const {
        return vds >= 0 ? channel(vgs, vds) : -channel(vgd, -vds);
    }
    void currents(const double* v, double* i) const override {  // D, G, S
        const double vgs = sign_ * (v[1] - v[2]), vgd = sign_ * (v[1] - v[0]), vds = sign_ * (v[0] - v[2]);
        const double id = ids(vgs, vgd, vds);
        const double igs = junction(is_, vgs, n_ * vt_), igd = junction(is_, vgd, n_ * vt_);
        i[0] = sign_ * (id - igd);
        i[1] = sign_ * (igs + igd);
        i[2] = sign_ * (-id - igs);
    }
    bool hasCharge() const override { return cgs_ > 0 || cgd_ > 0; }
    void charges(const double* v, double* q) const override {
        const double vgs = sign_ * (v[1] - v[2]), vgd = sign_ * (v[1] - v[0]);
        const double qgs = depletionCharge(cgs_, pb_, m_, fc_, vgs), qgd = depletionCharge(cgd_, pb_, m_, fc_, vgd);
        q[0] = -sign_ * qgd;
        q[1] = sign_ * (qgs + qgd);
        q[2] = -sign_ * qgs;
    }
    void noise(const double* v, std::vector<NoiseTerm>& out) const override {
        const double vgs = sign_ * (v[1] - v[2]), vgd = sign_ * (v[1] - v[0]), vds = sign_ * (v[0] - v[2]);
        const double h = 1e-6;
        const double gm = std::fabs(ids(vgs + h, vgd + h, vds) - ids(vgs - h, vgd - h, vds)) / (2 * h);
        const double id = std::fabs(ids(vgs, vgd, vds));
        out.push_back({0, 2, 8.0 / 3.0 * kBoltzmann * kNominalTemp * gm, kf_ * std::pow(id, af_), "thermal"});
    }

private:
    double sign_, vt_, vto_, beta_, lambda_, is_, n_, cgs_, cgd_, pb_, m_, fc_, kf_, af_;
};
}  // namespace

double depletionCharge(double cj0, double vj, double m, double fc, double v) {
    if (!(cj0 > 0)) return 0.0;
    const double fcv = fc * vj;
    if (v < fcv) return cj0 * vj / (1 - m) * (1 - std::pow(1 - v / vj, 1 - m));
    const double f1 = vj / (1 - m) * (1 - std::pow(1 - fc, 1 - m));
    const double f2 = std::pow(1 - fc, 1 + m);
    const double f3 = 1 - fc * (1 + m);
    return cj0 * (f1 + (f3 * (v - fcv) + m / (2 * vj) * (v * v - fcv * fcv)) / f2);
}

std::shared_ptr<const Device> makeDevice(const SpicePrimitive& p, std::string& error, double tempC) {
    switch (p.type) {
        case 'D': return std::make_shared<Diode>(p, tempC);
        case 'Q': return std::make_shared<Bjt>(p, tempC);
        case 'M': return std::make_shared<Mosfet>(p, tempC);
        case 'J': return std::make_shared<Jfet>(p, tempC);
        default: break;
    }
    error = p.name + ": not a semiconductor device.";
    return nullptr;
}

}  // namespace sieda::spicedev
