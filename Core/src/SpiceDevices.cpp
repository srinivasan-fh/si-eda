#include "SpiceDevices.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace sieda::spicedev {

namespace {
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

// ------------------------------------------------------------------ diode

class Diode final : public Device {
public:
    explicit Diode(const SpicePrimitive& p) {
        const double area = p.area * p.multiplier;
        is_ = param(p, {"IS"}, 1e-14) * area;
        n_ = std::max(param(p, {"N"}, 1.0), 0.05);
        cj0_ = param(p, {"CJO", "CJ0", "CJ"}, 0.0) * area;
        vj_ = std::max(param(p, {"VJ", "PB"}, 1.0), 0.05);
        m_ = std::clamp(param(p, {"M", "MJ"}, 0.5), 0.0, 0.99);
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
    double forward(double v) const { return junction(is_, v, n_ * kVtNominal); }
    double total(double v) const {
        double i = forward(v);
        if (std::isfinite(bv_)) {
            // Reverse breakdown: −IBV·exp(−(v + BV) / (NBV·Vt)), negligible above −BV.
            i -= junction(ibv_, -(v + bv_), nbv_ * kVtNominal) + ibv_;
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
    void noise(const double* v, std::vector<NoiseTerm>& out) const override {
        double id = std::fabs(total(v[0] - v[1]));
        out.push_back({0, 1, 2 * kCharge * id, kf_ * std::pow(id, af_), "shot"});
    }

private:
    double is_, n_, cj0_, vj_, m_, fc_, tt_, bv_, ibv_, nbv_, kf_, af_;
};

// ------------------------------------------------------------------ BJT (Gummel–Poon)

class Bjt final : public Device {
public:
    explicit Bjt(const SpicePrimitive& p) {
        sign_ = p.modelType == "PNP" ? -1.0 : 1.0;
        const double area = p.area * p.multiplier;
        is_ = param(p, {"IS"}, 1e-16) * area;
        bf_ = std::max(param(p, {"BF"}, 100.0), 1e-6);
        br_ = std::max(param(p, {"BR"}, 1.0), 1e-6);
        nf_ = std::max(param(p, {"NF"}, 1.0), 0.05);
        nr_ = std::max(param(p, {"NR"}, 1.0), 0.05);
        invVaf_ = safeInv(param(p, {"VAF", "VA"}, kInf));
        invVar_ = safeInv(param(p, {"VAR", "VB"}, kInf));
        invIkf_ = safeInv(param(p, {"IKF", "IK"}, kInf) * area);
        invIkr_ = safeInv(param(p, {"IKR"}, kInf) * area);
        ise_ = param(p, {"ISE"}, 0.0) * area;
        ne_ = std::max(param(p, {"NE"}, 1.5), 0.05);
        isc_ = param(p, {"ISC"}, 0.0) * area;
        nc_ = std::max(param(p, {"NC"}, 2.0), 0.05);
        cje_ = param(p, {"CJE"}, 0.0) * area;
        vje_ = std::max(param(p, {"VJE", "PE"}, 0.75), 0.05);
        mje_ = std::clamp(param(p, {"MJE", "ME"}, 0.33), 0.0, 0.99);
        cjc_ = param(p, {"CJC"}, 0.0) * area;
        vjc_ = std::max(param(p, {"VJC", "PC"}, 0.75), 0.05);
        mjc_ = std::clamp(param(p, {"MJC", "MC"}, 0.33), 0.0, 0.99);
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
        const double vt = kVtNominal;
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
    void noise(const double* v, std::vector<NoiseTerm>& out) const override {
        const double vbe = sign_ * (v[1] - v[2]), vbc = sign_ * (v[1] - v[0]);
        Op o = op(vbe, vbc);
        const double ic = std::fabs(o.ic), ib = std::fabs(o.ib);
        out.push_back({0, 2, 2 * kCharge * ic, 0.0, "shot"});
        out.push_back({1, 2, 2 * kCharge * ib, kf_ * std::pow(ib, af_), "shot"});
    }

private:
    double sign_, is_, bf_, br_, nf_, nr_, invVaf_, invVar_, invIkf_, invIkr_, ise_, ne_, isc_, nc_;
    double cje_, vje_, mje_, cjc_, vjc_, mjc_, tf_, tr_, fc_, kf_, af_;
};

// ------------------------------------------------------------------ MOSFET (levels 1 and 3)

class Mosfet final : public Device {
public:
    explicit Mosfet(const SpicePrimitive& p) {
        sign_ = p.modelType == "PMOS" ? -1.0 : 1.0;
        const double m = p.multiplier;
        const double ld = std::max(param(p, {"LD"}, 0.0), 0.0);
        leff_ = std::max(p.l - 2 * ld, p.l * 1e-3);
        w_ = p.w * m;
        tox_ = param(p, {"TOX"}, 0.0);
        cox_ = tox_ > 0 ? kEpsOx / tox_ : 0.0;
        double kp = param(p, {"KP"}, 0.0);
        if (!(kp > 0)) {
            double uo = param(p, {"UO", "U0"}, 0.0);
            kp = uo > 0 && cox_ > 0 ? uo * 1e-4 * cox_ : 2e-5;
        }
        beta_ = kp * w_ / leff_;
        vto_ = sign_ * param(p, {"VTO", "VT0"}, 0.0);
        gamma_ = std::max(param(p, {"GAMMA"}, 0.0), 0.0);
        phi_ = std::max(param(p, {"PHI"}, 0.6), 0.1);
        lambda_ = p.level == 3 ? 0.0 : param(p, {"LAMBDA"}, 0.0);
        if (p.level == 3 && p.model.count("LAMBDA")) lambda_ = param(p, {"LAMBDA"}, 0.0);
        theta_ = p.level == 3 ? std::max(param(p, {"THETA"}, 0.0), 0.0) : 0.0;
        is_ = param(p, {"IS"}, 1e-14) * m;
        pb_ = std::max(param(p, {"PB"}, 0.8), 0.05);
        mj_ = std::clamp(param(p, {"MJ"}, 0.5), 0.0, 0.99);
        mjsw_ = std::clamp(param(p, {"MJSW"}, 0.33), 0.0, 0.99);
        fc_ = std::clamp(param(p, {"FC"}, 0.5), 0.0, 0.95);
        const double cj = param(p, {"CJ"}, 0.0), cjsw = param(p, {"CJSW"}, 0.0);
        cbd_ = param(p, {"CBD"}, 0.0) * m;
        cbs_ = param(p, {"CBS"}, 0.0) * m;
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
        const double vgst = vgs - threshold(vbs);
        if (vgst <= 0) return 0.0;
        const double beta = beta_ / (1.0 + theta_ * vgst);
        if (vds < vgst) return beta * (vgst - 0.5 * vds) * vds * (1.0 + lambda_ * vds);
        return 0.5 * beta * vgst * vgst * (1.0 + lambda_ * vds);
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
        const double ibd = junction(is_, b.vbd, kVtNominal), ibs = junction(is_, b.vbs, kVtNominal);
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
    double sign_, leff_, w_, tox_, cox_, beta_, vto_, gamma_, phi_, lambda_, theta_, is_, pb_, mj_, mjsw_, fc_;
    double cbd_, cbs_, cbdArea_, cbsArea_, cbdSw_, cbsSw_, cgs_, cgd_, cgb_, cint_, kf_, af_;
};

// ------------------------------------------------------------------ JFET

class Jfet final : public Device {
public:
    explicit Jfet(const SpicePrimitive& p) {
        sign_ = p.modelType == "PJF" ? -1.0 : 1.0;
        const double area = p.area * p.multiplier;
        vto_ = param(p, {"VTO", "VT0"}, -2.0);
        beta_ = param(p, {"BETA"}, 1e-4) * area;
        lambda_ = param(p, {"LAMBDA"}, 0.0);
        is_ = param(p, {"IS"}, 1e-14) * area;
        n_ = std::max(param(p, {"N"}, 1.0), 0.05);
        cgs_ = param(p, {"CGS"}, 0.0) * area;
        cgd_ = param(p, {"CGD"}, 0.0) * area;
        pb_ = std::max(param(p, {"PB"}, 1.0), 0.05);
        m_ = std::clamp(param(p, {"M"}, 0.5), 0.0, 0.99);
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
        const double igs = junction(is_, vgs, n_ * kVtNominal), igd = junction(is_, vgd, n_ * kVtNominal);
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
    double sign_, vto_, beta_, lambda_, is_, n_, cgs_, cgd_, pb_, m_, fc_, kf_, af_;
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

std::shared_ptr<const Device> makeDevice(const SpicePrimitive& p, std::string& error) {
    switch (p.type) {
        case 'D': return std::make_shared<Diode>(p);
        case 'Q': return std::make_shared<Bjt>(p);
        case 'M': return std::make_shared<Mosfet>(p);
        case 'J': return std::make_shared<Jfet>(p);
        default: break;
    }
    error = p.name + ": not a semiconductor device.";
    return nullptr;
}

}  // namespace sieda::spicedev
