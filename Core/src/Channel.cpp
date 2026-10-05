#include "sieda/Channel.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>

#include "sieda/Eye.hpp"
#include "sieda/LengthMatch.hpp"
#include "sieda/Project.hpp"
#include "sieda/SignalIntegrity.hpp"
#include "sieda/Stackup.hpp"
#include "sieda/Touchstone.hpp"

namespace sieda {

// ---- dense matrices ----------------------------------------------------------------------------------------------------

CMat CMat::identity(int size) {
    CMat m(size);
    for (int i = 0; i < size; ++i) m(i, i) = 1;
    return m;
}

CMat operator*(const CMat& x, const CMat& y) {
    CMat r(x.n);
    for (int i = 0; i < x.n; ++i)
        for (int k = 0; k < x.n; ++k) {
            const cplx v = x(i, k);
            if (v == cplx(0, 0)) continue;
            for (int j = 0; j < x.n; ++j) r(i, j) += v * y(k, j);
        }
    return r;
}

CMat operator+(const CMat& x, const CMat& y) {
    CMat r = x;
    for (size_t k = 0; k < r.a.size(); ++k) r.a[k] += y.a[k];
    return r;
}

CMat operator-(const CMat& x, const CMat& y) {
    CMat r = x;
    for (size_t k = 0; k < r.a.size(); ++k) r.a[k] -= y.a[k];
    return r;
}

bool invertInPlace(CMat& m) {
    const int n = m.n;
    CMat inv = CMat::identity(n);
    for (int c = 0; c < n; ++c) {
        int piv = c;
        double best = std::abs(m(c, c));
        for (int r = c + 1; r < n; ++r)
            if (std::abs(m(r, c)) > best) {
                best = std::abs(m(r, c));
                piv = r;
            }
        if (!(best > 1e-300) || !std::isfinite(best)) return false;
        if (piv != c)
            for (int j = 0; j < n; ++j) {
                std::swap(m(c, j), m(piv, j));
                std::swap(inv(c, j), inv(piv, j));
            }
        const cplx d = 1.0 / m(c, c);
        for (int j = 0; j < n; ++j) {
            m(c, j) *= d;
            inv(c, j) *= d;
        }
        for (int r = 0; r < n; ++r) {
            if (r == c) continue;
            const cplx f = m(r, c);
            if (f == cplx(0, 0)) continue;
            for (int j = 0; j < n; ++j) {
                m(r, j) -= f * m(c, j);
                inv(r, j) -= f * inv(c, j);
            }
        }
    }
    m = inv;
    return true;
}

CMat block(const CMat& m, int r0, int c0, int size) {
    CMat b(size);
    for (int i = 0; i < size; ++i)
        for (int j = 0; j < size; ++j) b(i, j) = m(r0 + i, c0 + j);
    return b;
}

// ---- network -----------------------------------------------------------------------------------------------------------

std::pair<cplx, cplx> lineAdmittance(const LineModel& m, double lengthM, double f) {
    const double len = std::max(1e-9, lengthM);
    const cplx Z = m.seriesZ(f), Y = m.shuntY(f);
    cplx g = std::sqrt(Z * Y);
    if (g.real() < 0) g = -g;
    const cplx x = g * len;
    if (std::abs(x) < 1e-4) {
        // coth x ≈ 1/x + x/3, csch x ≈ 1/x − x/6 (Zc·x = Z·len, x/Zc = Y·len).
        return {1.0 / (Z * len) + Y * len / 3.0, -1.0 / (Z * len) + Y * len / 6.0};
    }
    const cplx zc = Z / g;
    const cplx e = std::exp(-x), e2 = e * e;
    const cplx den = (1.0 - e2) * zc;
    return {(1.0 + e2) / den, -2.0 * e / den};
}

namespace {

/// Symmetric sparse nodal matrix with a current vector; eliminates nodes in a fixed order.
struct Sparse {
    std::vector<cplx> diag, cur;
    std::vector<std::vector<std::pair<int, cplx>>> off;
    std::vector<char> gone;
    explicit Sparse(int n) : diag(static_cast<size_t>(n), cplx(1e-12, 0)), cur(static_cast<size_t>(n)), off(static_cast<size_t>(n)),
                             gone(static_cast<size_t>(n), 0) {}
    void addOff(int i, int j, cplx v) {
        for (auto& e : off[static_cast<size_t>(i)])
            if (e.first == j) {
                e.second += v;
                return;
            }
        off[static_cast<size_t>(i)].push_back({j, v});
    }
    void add(int i, int j, cplx v) {
        if (i == j) {
            diag[static_cast<size_t>(i)] += v;
            return;
        }
        addOff(i, j, v);
        addOff(j, i, v);
    }
    void stamp2(int a, int b, std::pair<cplx, cplx> y) {
        add(a, a, y.first);
        add(b, b, y.first);
        add(a, b, y.second);
    }
    void eliminate(int k) {
        const size_t K = static_cast<size_t>(k);
        const cplx d = diag[K];
        std::vector<std::pair<int, cplx>> nb;
        for (const auto& e : off[K])
            if (!gone[static_cast<size_t>(e.first)] && e.first != k) nb.push_back(e);
        gone[K] = 1;
        if (std::abs(d) < 1e-300) return;
        for (size_t p = 0; p < nb.size(); ++p) {
            const auto [i, yi] = nb[p];
            diag[static_cast<size_t>(i)] -= yi * yi / d;
            cur[static_cast<size_t>(i)] -= yi * cur[K] / d;
            for (size_t q = p + 1; q < nb.size(); ++q) {
                const auto [j, yj] = nb[q];
                const cplx v = -yi * yj / d;
                addOff(i, j, v);
                addOff(j, i, v);
            }
        }
    }
    cplx entry(int i, int j) const {
        if (i == j) return diag[static_cast<size_t>(i)];
        for (const auto& e : off[static_cast<size_t>(i)])
            if (e.first == j) return e.second;
        return 0;
    }
};

/// Minimum-degree elimination order of every node not in `keep`.
std::vector<int> eliminationOrder(const ChannelNetwork& net, const std::vector<int>& keep) {
    const size_t n = static_cast<size_t>(net.nodes);
    std::vector<std::set<int>> adj(n);
    auto link = [&](int a, int b) {
        if (a == b || a < 0 || b < 0) return;
        adj[static_cast<size_t>(a)].insert(b);
        adj[static_cast<size_t>(b)].insert(a);
    };
    for (const auto& l : net.lines) link(l.a, l.b);
    for (const auto& c : net.coupled) {
        const int v[4] = {c.a1, c.b1, c.a2, c.b2};
        for (int i = 0; i < 4; ++i)
            for (int j = i + 1; j < 4; ++j) link(v[i], v[j]);
    }
    std::vector<char> kept(n, 0), done(n, 0);
    for (int k : keep)
        if (k >= 0 && static_cast<size_t>(k) < n) kept[static_cast<size_t>(k)] = 1;
    std::vector<int> order;
    std::set<std::pair<size_t, int>> queue;
    for (size_t i = 0; i < n; ++i)
        if (!kept[i]) queue.insert({adj[i].size(), static_cast<int>(i)});
    while (!queue.empty()) {
        const int k = queue.begin()->second;
        queue.erase(queue.begin());
        const size_t K = static_cast<size_t>(k);
        done[K] = 1;
        order.push_back(k);
        std::vector<int> nb(adj[K].begin(), adj[K].end());
        for (int i : nb) {
            const size_t I = static_cast<size_t>(i);
            if (!kept[I] && !done[I]) queue.erase({adj[I].size(), i});
            adj[I].erase(k);
        }
        for (size_t p = 0; p < nb.size(); ++p)
            for (size_t q = p + 1; q < nb.size(); ++q) {
                adj[static_cast<size_t>(nb[p])].insert(nb[q]);
                adj[static_cast<size_t>(nb[q])].insert(nb[p]);
            }
        for (int i : nb) {
            const size_t I = static_cast<size_t>(i);
            if (!kept[I] && !done[I]) queue.insert({adj[I].size(), i});
        }
    }
    return order;
}

Sparse stampNetwork(const ChannelNetwork& net, double f) {
    Sparse sp(net.nodes);
    const double w = 2 * kPi * f;
    for (const auto& l : net.lines) {
        if (l.a < 0 || l.b < 0 || l.a >= net.nodes || l.b >= net.nodes || l.a == l.b) continue;
        sp.stamp2(l.a, l.b, lineAdmittance(l.model, l.length, f));
    }
    for (const auto& c : net.coupled) {
        const auto ye = lineAdmittance(c.even, c.length, f), yo = lineAdmittance(c.odd, c.length, f);
        const cplx s11 = (ye.first + yo.first) / 2.0, m11 = (ye.first - yo.first) / 2.0;
        const cplx s12 = (ye.second + yo.second) / 2.0, m12 = (ye.second - yo.second) / 2.0;
        for (int n : {c.a1, c.b1, c.a2, c.b2}) sp.add(n, n, s11);
        sp.add(c.a1, c.a2, m11);
        sp.add(c.b1, c.b2, m11);
        sp.add(c.a1, c.b1, s12);
        sp.add(c.a2, c.b2, s12);
        sp.add(c.a1, c.b2, m12);
        sp.add(c.a2, c.b1, m12);
    }
    for (const auto& s : net.shunts) {
        if (s.node < 0 || s.node >= net.nodes) continue;
        sp.add(s.node, s.node, cplx(s.g, w * s.c));
    }
    return sp;
}

}  // namespace

std::vector<CMat> ChannelNetwork::portAdmittance(const std::vector<double>& freq) const {
    std::vector<CMat> out;
    const int P = static_cast<int>(ports.size());
    if (P == 0 || nodes == 0) return out;
    const std::vector<int> order = eliminationOrder(*this, ports);
    out.reserve(freq.size());
    for (double f : freq) {
        Sparse sp = stampNetwork(*this, std::max(0.0, f));
        for (int k : order) sp.eliminate(k);
        CMat y(P);
        for (int i = 0; i < P; ++i)
            for (int j = 0; j < P; ++j) y(i, j) = sp.entry(ports[static_cast<size_t>(i)], ports[static_cast<size_t>(j)]);
        out.push_back(y);
    }
    return out;
}

std::vector<CMat> ChannelNetwork::sParameters(const std::vector<double>& freq, double z0) const {
    std::vector<CMat> out;
    const int P = static_cast<int>(ports.size());
    for (const CMat& y : portAdmittance(freq)) {
        CMat zy(P);
        for (size_t k = 0; k < zy.a.size(); ++k) zy.a[k] = z0 * y.a[k];
        CMat left = CMat::identity(P) + zy;
        CMat s = CMat::identity(P) - zy;
        if (!invertInPlace(left)) {
            out.push_back(CMat(P));
            continue;
        }
        out.push_back(left * s);
    }
    return out;
}

std::vector<double> ChannelNetwork::dcVoltages(const std::vector<Shunt>& extra, const std::vector<int>& observe) const {
    ChannelNetwork net = *this;
    for (const auto& e : extra) net.shunts.push_back(e);
    Sparse sp = stampNetwork(net, 0.0);
    for (const auto& s : net.shunts)
        if (s.node >= 0 && s.node < net.nodes) sp.cur[static_cast<size_t>(s.node)] += s.j;
    for (int k : eliminationOrder(net, observe)) sp.eliminate(k);
    const int P = static_cast<int>(observe.size());
    CMat y(P);
    for (int i = 0; i < P; ++i)
        for (int j = 0; j < P; ++j) y(i, j) = sp.entry(observe[static_cast<size_t>(i)], observe[static_cast<size_t>(j)]);
    std::vector<double> v(static_cast<size_t>(P), 0.0);
    if (!invertInPlace(y)) return v;
    for (int i = 0; i < P; ++i) {
        cplx s = 0;
        for (int j = 0; j < P; ++j) s += y(i, j) * sp.cur[static_cast<size_t>(observe[static_cast<size_t>(j)])];
        v[static_cast<size_t>(i)] = s.real();
    }
    return v;
}

// ---- S-parameter utilities ------------------------------------------------------------------------------------------------

CMat mixedMode(const CMat& s4) {
    const double r = 1 / std::sqrt(2.0);
    CMat M(4);
    M(0, 0) = r, M(0, 1) = -r;
    M(1, 2) = r, M(1, 3) = -r;
    M(2, 0) = r, M(2, 1) = r;
    M(3, 2) = r, M(3, 3) = r;
    CMat Mt(4);
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) Mt(i, j) = M(j, i);
    return M * s4 * Mt;
}

CMat cascadeStar(const CMat& a, const CMat& b) {
    const int N = a.n / 2;
    const CMat A11 = block(a, 0, 0, N), A12 = block(a, 0, N, N), A21 = block(a, N, 0, N), A22 = block(a, N, N, N);
    const CMat B11 = block(b, 0, 0, N), B12 = block(b, 0, N, N), B21 = block(b, N, 0, N), B22 = block(b, N, N, N);
    const CMat I = CMat::identity(N);
    CMat X = I - B11 * A22;  // (I − B11·A22)⁻¹
    CMat Y = I - A22 * B11;  // (I − A22·B11)⁻¹
    CMat out(2 * N);
    if (!invertInPlace(X) || !invertInPlace(Y)) return out;
    const CMat S11 = A11 + A12 * X * B11 * A21, S12 = A12 * X * B12, S21 = B21 * Y * A21, S22 = B22 + B21 * Y * A22 * B12;
    for (int i = 0; i < N; ++i)
        for (int j = 0; j < N; ++j) {
            out(i, j) = S11(i, j);
            out(i, j + N) = S12(i, j);
            out(i + N, j) = S21(i, j);
            out(i + N, j + N) = S22(i, j);
        }
    return out;
}

SParams interpolateSParams(const SParams& src, const std::vector<double>& freq) {
    SParams out;
    out.ports = src.ports;
    out.z0 = src.z0;
    out.freq = freq;
    const size_t F = src.freq.size();
    const int P = src.ports;
    out.s.assign(freq.size(), CMat(P));
    if (F == 0 || P <= 0) return out;
    std::vector<double> mag(F), ph(F);
    for (int i = 0; i < P; ++i)
        for (int j = 0; j < P; ++j) {
            for (size_t k = 0; k < F; ++k) {
                const cplx v = src.s[k](i, j);
                mag[k] = std::abs(v);
                double p = std::arg(v);
                if (k > 0) {
                    while (p - ph[k - 1] > kPi) p -= 2 * kPi;
                    while (p - ph[k - 1] < -kPi) p += 2 * kPi;
                }
                ph[k] = p;
            }
            for (size_t q = 0; q < freq.size(); ++q) {
                const double f = freq[q];
                double m, p;
                if (F == 1 || f <= src.freq.front()) {
                    m = mag.front();
                    p = src.freq.front() > 0 ? ph.front() * std::max(0.0, f) / src.freq.front() : ph.front();
                } else if (f >= src.freq.back()) {
                    const double slope = (ph[F - 1] - ph[F - 2]) / std::max(1e-30, src.freq[F - 1] - src.freq[F - 2]);
                    m = mag.back();
                    p = ph.back() + slope * (f - src.freq.back());
                } else {
                    const size_t hi = static_cast<size_t>(std::upper_bound(src.freq.begin(), src.freq.end(), f) - src.freq.begin());
                    const size_t lo = hi - 1;
                    const double t = (f - src.freq[lo]) / std::max(1e-30, src.freq[hi] - src.freq[lo]);
                    m = mag[lo] + t * (mag[hi] - mag[lo]);
                    p = ph[lo] + t * (ph[hi] - ph[lo]);
                }
                out.s[q](i, j) = std::polar(m, p);
            }
        }
    return out;
}

PortTermination PortTermination::source(cplx vth, cplx zth, double z0) {
    PortTermination t;
    t.gamma = (zth - z0) / (zth + z0);
    t.c = vth * std::sqrt(z0) / (zth + z0);
    return t;
}

PortTermination PortTermination::load(cplx y, double z0) {
    PortTermination t;
    t.gamma = (1.0 - z0 * y) / (1.0 + z0 * y);
    return t;
}

std::vector<cplx> terminatedVoltages(const CMat& s, double z0, const std::vector<PortTermination>& t) {
    const int n = s.n;
    std::vector<cplx> v(static_cast<size_t>(n), cplx(0, 0));
    if (static_cast<int>(t.size()) != n) return v;
    CMat m = CMat::identity(n);
    for (int i = 0; i < n; ++i)
        for (int j = 0; j < n; ++j) m(i, j) -= s(i, j) * t[static_cast<size_t>(j)].gamma;
    if (!invertInPlace(m)) return v;
    std::vector<cplx> rhs(static_cast<size_t>(n), cplx(0, 0)), b(static_cast<size_t>(n), cplx(0, 0));
    for (int i = 0; i < n; ++i)
        for (int j = 0; j < n; ++j) rhs[static_cast<size_t>(i)] += s(i, j) * t[static_cast<size_t>(j)].c;
    for (int i = 0; i < n; ++i)
        for (int j = 0; j < n; ++j) b[static_cast<size_t>(i)] += m(i, j) * rhs[static_cast<size_t>(j)];
    const double r = std::sqrt(z0);
    for (int i = 0; i < n; ++i) {
        const size_t I = static_cast<size_t>(i);
        const cplx a = t[I].gamma * b[I] + t[I].c;
        v[I] = r * (a + b[I]);
    }
    return v;
}

void fft(std::vector<cplx>& x, bool inverse) {
    const size_t n = x.size();
    if (n < 2) return;
    for (size_t i = 1, j = 0; i < n; ++i) {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(x[i], x[j]);
    }
    for (size_t len = 2; len <= n; len <<= 1) {
        const double ang = 2 * kPi / static_cast<double>(len) * (inverse ? 1 : -1);
        const cplx wl(std::cos(ang), std::sin(ang));
        for (size_t i = 0; i < n; i += len) {
            cplx w(1, 0);
            for (size_t k = 0; k < len / 2; ++k) {
                const cplx u = x[i + k], v = x[i + k + len / 2] * w;
                x[i + k] = u + v;
                x[i + k + len / 2] = u - v;
                w *= wl;
            }
        }
    }
    if (inverse)
        for (auto& v : x) v /= static_cast<double>(n);
}

// ---- routed-net extraction ------------------------------------------------------------------------------------------------

namespace {

int findNet(const Schematic& sch, const std::string& name) {
    for (const auto& n : sch.nets())
        if (n.name == name) return n.index;
    return -1;
}

/// A via barrel section as an ideal line (Johnson & Graham L and C, as in the reflection analysis).
LineModel viaLine(const BoardSettings& s, const NetCopperGraph::Edge& e) {
    const double er = boardLaminate(s).er;
    const double c = viaCapacitance(s.thickness, e.diameter, e.diameter + 2 * s.clearance, er) / s.thickness;  // F/mm
    const double l = viaInductance(s.thickness, e.width) / s.thickness;                                      // H/mm
    const double z = c > 0 && l > 0 ? std::sqrt(l / c) : 50;
    const double tpd = c > 0 && l > 0 ? std::sqrt(l * c) : 7e-12;  // s/mm
    return LineModel::ideal(z, tpd * 1e3);
}

/// Even / odd mode line of a coupled pair from the coupling coefficients: L_e,o = L(1 ± kl), C_e,o = C11(1 ∓ kc) with
/// C11 = C/(1 − kl·kc) (exact for a homogeneous dielectric, where kl = kc and both modes travel at c/√εr).
LineModel modeLine(const LineModel& base, double kl, double kc, int sign) {
    LineModel m = base;
    const double norm = 1 - kl * kc;
    const double lf = 1 + sign * kl, cf = (1 - sign * kc) / norm;
    m.z0 = base.z0 * std::sqrt(lf / cf);
    m.epsEff = std::clamp(base.epsEff * lf * cf, 1.0, std::max(1.0, base.er));
    m.fill = base.stripline || base.er - 1 < 1e-9 ? 1.0 : std::clamp((m.epsEff - 1) / (base.er - 1), 0.0, 1.0);
    const double se = std::sqrt(m.epsEff);
    m.lExt = m.z0 * se / kSpeedOfLight;
    m.cAir = 1 / (m.z0 * se * kSpeedOfLight);
    return m;
}

struct Seg {
    int a = 0, b = 0;
    Vec2 pa, pb;
    int layer = 0;
    double width = 0, length = 0, diameter = 0;
    bool via = false;
    int leg = 0;
    size_t edge = 0;
};

struct Coupling {
    size_t sp = 0, sn = 0;
    double p0 = 0, p1 = 0, n0 = 0, n1 = 0;  // parameters on sp / sn
    double gap = 0;
};

double paramOn(const Seg& s, Vec2 x) {
    const Vec2 d = s.pb - s.pa;
    const double l2 = d.dot(d);
    return l2 > 0 ? (x - s.pa).dot(d) / l2 : 0;
}

}  // namespace

ChannelOptions channelOptionsFromJson(const Json& j) {
    ChannelOptions o;
    o.net = j.get("net").asString("");
    o.partner = j.get("partner").asString("");
    o.receiver = j.get("receiver").asString("");
    o.fMax = std::clamp(j.get("fMax").asNumber(20e9), 1e8, 110e9);
    o.points = std::clamp(j.get("points").asInt(401), 11, 4001);
    o.refOhms = std::clamp(j.get("refOhms").asNumber(50), 1.0, 1000.0);
    o.loss.foil = j.get("foil").asString("");
    o.loss.roughness = roughnessFromString(j.get("roughness").asString("huray"));
    o.loss.lossless = j.get("lossless").asBool(false);
    return o;
}

ChannelDrive channelDriveFromJson(const Json& j) {
    ChannelDrive d;
    d.idealDriver = j.get("driver").asString("model") == "ideal";
    d.swing = std::clamp(j.get("swing").asNumber(1.0), 0.01, 20.0);
    d.riseTime = std::clamp(j.get("riseTime").asNumber(0), 0.0, 1e-6);
    d.sourceOhms = std::clamp(j.get("sourceOhms").asNumber(50), 1.0, 1000.0);
    d.termOhms = std::clamp(j.get("termOhms").asNumber(50), 1.0, 1e6);
    return d;
}

ChannelModel extractChannel(const Project& project, const ChannelOptions& opt) {
    ChannelModel m;
    const Schematic& sch = project.schematic;
    const BoardSettings& s = project.pcb.settings;
    int netP = findNet(sch, opt.net);
    if (netP < 0) {
        m.error = "Unknown net " + opt.net;
        return m;
    }
    int netN = -1;
    if (opt.partner != "none") {
        if (!opt.partner.empty()) {
            netN = findNet(sch, opt.partner);
            if (netN < 0) {
                m.error = "Unknown net " + opt.partner;
                return m;
            }
        } else {
            for (const auto& [a, b] : differentialPairs(sch)) {
                if (a != netP && b != netP) continue;
                netP = a;  // P first
                netN = b;
                break;
            }
        }
    }
    m.differential = netN >= 0 && netN != netP;
    m.netP = sch.nets()[static_cast<size_t>(netP)].name;
    if (m.differential) m.netN = sch.nets()[static_cast<size_t>(netN)].name;

    SiNetCircuit circ[2];
    SiNetResult res[2];
    const int legs = m.differential ? 2 : 1;
    for (int k = 0; k < legs; ++k) {
        res[k] = analyzeNetCircuit(project, k == 0 ? netP : netN, circ[k]);
        if (!res[k].error.empty()) {
            m.error = (k == 0 ? m.netP : m.netN) + ": " + res[k].error;
            return m;
        }
        m.estimated = m.estimated || res[k].estimated;
    }
    // Receivers: the named one, else the farthest; the N leg uses the same part when it can.
    int rx[2] = {-1, -1};
    for (int k = 0; k < legs; ++k) {
        double best = -1;
        for (size_t i = 0; i < circ[k].receivers.size(); ++i) {
            const auto& cr = circ[k].receivers[i];
            const SiReceiver& sr = res[k].receivers[cr.result];
            if (!sr.connected) continue;
            double score = sr.pathDelay;
            if (!opt.receiver.empty() && cr.ref == opt.receiver) score += 1;
            if (k == 1 && rx[0] >= 0 && cr.componentId == circ[0].receivers[static_cast<size_t>(rx[0])].componentId) score += 2;
            if (score > best) {
                best = score;
                rx[k] = static_cast<int>(i);
            }
        }
        if (rx[k] < 0) {
            m.error = (k == 0 ? m.netP : m.netN) + ": no logic receiver on the net";
            return m;
        }
    }
    const SiNetCircuit::Receiver& rxP = circ[0].receivers[static_cast<size_t>(rx[0])];
    m.driver = res[0].driver;
    m.seriesR = res[0].seriesR;
    m.driverRef = res[0].driverRef;
    m.driverPin = res[0].driverPin;
    m.receiver = rxP.model;
    m.receiverRef = rxP.ref;
    m.receiverPin = rxP.pin;
    m.lengthP = res[0].receivers[rxP.result].pathLength;
    m.delayP = res[0].receivers[rxP.result].pathDelay;
    m.vias = res[0].vias;
    if (m.differential) {
        const auto& rxN = circ[1].receivers[static_cast<size_t>(rx[1])];
        m.lengthN = res[1].receivers[rxN.result].pathLength;
        m.delayN = res[1].receivers[rxN.result].pathDelay;
        m.vias += res[1].vias;
        m.receiverPin += " / " + rxN.pin;
        m.driverPin += " / " + res[1].driverPin;
        if (rxN.componentId != rxP.componentId) m.notes.push_back("The P and N legs end at different parts");
    }
    if (m.estimated) m.notes.push_back("Not fully routed: analysed on straight-line lengths");

    // Nodes and segments of both legs.
    ChannelNetwork& net = m.network;
    std::vector<Vec2> pos;
    std::vector<Seg> segs;
    int offset[2] = {0, 0};
    for (int k = 0; k < legs; ++k) {
        offset[k] = net.nodes;
        const NetCopperGraph& g = circ[k].graph;
        for (const auto& nd : g.nodes) {
            net.addNode();
            pos.push_back(nd.p);
        }
        for (size_t e = 0; e < g.edges.size(); ++e) {
            const auto& ed = g.edges[e];
            if (!circ[k].reach[static_cast<size_t>(ed.a)]) continue;
            Seg sg;
            sg.a = ed.a + offset[k];
            sg.b = ed.b + offset[k];
            sg.pa = g.nodes[static_cast<size_t>(ed.a)].p;
            sg.pb = g.nodes[static_cast<size_t>(ed.b)].p;
            sg.layer = ed.layer;
            sg.width = ed.width;
            sg.length = ed.length;
            sg.diameter = ed.diameter;
            sg.via = ed.via;
            sg.leg = k;
            sg.edge = e;
            segs.push_back(sg);
        }
    }

    // Coupled sections of a pair: P and N tracks side by side on one layer.
    std::vector<Coupling> couplings;
    std::vector<std::vector<double>> cuts(segs.size());
    if (m.differential) {
        for (size_t i = 0; i < segs.size(); ++i) {
            const Seg& a = segs[i];
            if (a.leg != 0 || a.via) continue;
            const Vec2 da = a.pb - a.pa;
            const double la = da.length();
            if (la < 0.2) continue;
            const Vec2 u = da * (1.0 / la);
            const double reach = 6 * std::max(0.05, impedanceReferenceHeight(s, a.layer)) + a.width;
            for (size_t j = 0; j < segs.size(); ++j) {
                const Seg& b = segs[j];
                if (b.leg != 1 || b.via || b.layer != a.layer) continue;
                const Vec2 db = b.pb - b.pa;
                const double lb = db.length();
                if (lb < 0.2 || std::fabs(u.dot(db) / lb) < 0.97) continue;
                const double t0 = (b.pa - a.pa).dot(u), t1 = (b.pb - a.pa).dot(u);
                const double o0 = std::max(0.0, std::min(t0, t1)), o1 = std::min(la, std::max(t0, t1));
                if (o1 - o0 < 0.2) continue;
                auto perp = [&](Vec2 p) { return std::fabs((p - a.pa).x * u.y - (p - a.pa).y * u.x); };
                const Vec2 x0 = a.pa + u * o0, x1 = a.pa + u * o1;
                const double n0 = paramOn(b, x0), n1 = paramOn(b, x1);
                const Vec2 y0 = b.pa + db * n0, y1 = b.pa + db * n1;
                const double gap = 0.5 * (perp(y0) + perp(y1)) - (a.width + b.width) / 2;
                if (gap < 0 || gap > reach) continue;
                Coupling c;
                c.sp = i;
                c.sn = j;
                c.p0 = o0 / la;
                c.p1 = o1 / la;
                c.n0 = n0;
                c.n1 = n1;
                c.gap = gap;
                couplings.push_back(c);
                cuts[i].push_back(c.p0);
                cuts[i].push_back(c.p1);
                cuts[j].push_back(c.n0);
                cuts[j].push_back(c.n1);
            }
        }
        // Mirror the cuts inside each coupled span so the sub-sections pair up one to one.
        for (const auto& c : couplings) {
            const std::vector<double> cp = cuts[c.sp], cn = cuts[c.sn];
            for (double x : cp)
                if (x > c.p0 + 1e-9 && x < c.p1 - 1e-9) cuts[c.sn].push_back(c.n0 + (x - c.p0) / (c.p1 - c.p0) * (c.n1 - c.n0));
            const double nlo = std::min(c.n0, c.n1), nhi = std::max(c.n0, c.n1);
            for (double x : cn)
                if (x > nlo + 1e-9 && x < nhi - 1e-9) cuts[c.sp].push_back(c.p0 + (x - c.n0) / (c.n1 - c.n0) * (c.p1 - c.p0));
        }
    }
    // Split segments at their cuts.
    struct Sub {
        int a, b;
        double u0, u1, length;
        bool used = false;
    };
    std::vector<std::vector<Sub>> subs(segs.size());
    for (size_t i = 0; i < segs.size(); ++i) {
        const Seg& sg = segs[i];
        std::vector<double> c;
        for (double x : cuts[i])
            if (x > 1e-6 && x < 1 - 1e-6) c.push_back(x);
        std::sort(c.begin(), c.end());
        std::vector<double> pts{0.0};
        for (double x : c)
            if ((x - pts.back()) * std::max(1e-9, sg.length) > 1e-3) pts.push_back(x);
        if ((1 - pts.back()) * std::max(1e-9, sg.length) <= 1e-3 && pts.size() > 1) pts.pop_back();
        pts.push_back(1.0);
        int prev = sg.a;
        for (size_t k = 1; k < pts.size(); ++k) {
            int node;
            if (k + 1 == pts.size()) node = sg.b;
            else {
                node = net.addNode();
                pos.push_back(sg.pa + (sg.pb - sg.pa) * pts[k]);
            }
            subs[i].push_back({prev, node, pts[k - 1], pts[k], sg.length * (pts[k] - pts[k - 1])});
            prev = node;
        }
    }
    // Coupled elements.
    for (const auto& c : couplings) {
        const Seg& a = segs[c.sp];
        const Seg& b = segs[c.sn];
        auto toN = [&](double x) { return c.n0 + (x - c.p0) / (c.p1 - c.p0) * (c.n1 - c.n0); };
        for (auto& sa : subs[c.sp]) {
            const double mid = 0.5 * (sa.u0 + sa.u1);
            if (sa.used || mid < c.p0 || mid > c.p1) continue;
            const double nm = toN(mid);
            for (auto& sb : subs[c.sn]) {
                if (sb.used || nm < sb.u0 || nm > sb.u1) continue;
                sa.used = sb.used = true;
                const bool forward = std::fabs(toN(sa.u0) - sb.u0) <= std::fabs(toN(sa.u0) - sb.u1);
                ChannelNetwork::Coupled cl;
                cl.a1 = sa.a;
                cl.b1 = sa.b;
                cl.a2 = forward ? sb.a : sb.b;
                cl.b2 = forward ? sb.b : sb.a;
                const double lenMm = 0.5 * (sa.length + sb.length);
                cl.length = lenMm * 1e-3;
                const double w = 0.5 * (a.width + b.width);
                const LineModel base = lineModel(s, a.layer, w, opt.loss);
                const CouplingEstimate ce = crosstalkCoupling(s, a.layer, a.width, b.width, std::max(0.01, c.gap), lenMm, 1e-10);
                cl.even = modeLine(base, ce.kl, ce.kc, +1);
                cl.odd = modeLine(base, ce.kl, ce.kc, -1);
                net.coupled.push_back(cl);
                ChannelModel::CoupledSection cs;
                cs.layer = copperLayerName(a.layer, s.layerCount);
                cs.length = lenMm;
                cs.gap = c.gap;
                cs.zEven = cl.even.z0;
                cs.zOdd = cl.odd.z0;
                cs.zDiff = 2 * cl.odd.z0;
                cs.zComm = cl.even.z0 / 2;
                cs.epsEven = cl.even.epsEff;
                cs.epsOdd = cl.odd.epsEff;
                m.coupledLength += lenMm;
                m.coupled.push_back(cs);
                break;
            }
        }
    }
    // Plain lines.
    for (size_t i = 0; i < segs.size(); ++i) {
        const Seg& sg = segs[i];
        NetCopperGraph::Edge e;
        e.width = sg.width;
        e.diameter = sg.diameter;
        const LineModel lm = sg.via ? viaLine(s, e) : lineModel(s, sg.layer, sg.width, opt.loss);
        for (const auto& sb : subs[i]) {
            if (sb.used) continue;
            ChannelNetwork::Line l;
            l.a = sb.a;
            l.b = sb.b;
            l.model = lm;
            l.length = std::max(1e-6, sb.length) * 1e-3;
            net.lines.push_back(l);
        }
    }
    // Merge consecutive coupled sections into one report row per layer and gap.
    {
        std::vector<ChannelModel::CoupledSection> merged;
        for (const auto& cs : m.coupled) {
            auto it = std::find_if(merged.begin(), merged.end(), [&](const ChannelModel::CoupledSection& x) {
                return x.layer == cs.layer && std::fabs(x.gap - cs.gap) < 1e-3;
            });
            if (it == merged.end()) merged.push_back(cs);
            else it->length += cs.length;
        }
        m.coupled = merged;
    }
    // Passive loads, and the receivers that are not ports.
    for (int k = 0; k < legs; ++k) {
        const SiNetCircuit& c = circ[k];
        for (size_t n = 0; n < c.padC.size() && n < c.graph.nodes.size(); ++n) {
            if (!(c.padC[n] > 0) && !(c.padG[n] > 0)) continue;
            net.shunts.push_back({static_cast<int>(n) + offset[k], c.padG[n], c.padC[n], c.padJ[n]});
        }
        for (size_t i = 0; i < c.receivers.size(); ++i) {
            if (static_cast<int>(i) == rx[k]) continue;
            const auto& r = c.receivers[i];
            ChannelNetwork::Shunt sh;
            sh.node = r.node + offset[k];
            sh.c = r.model.cIn + r.model.cPkg;
            if (r.model.rTerm > 0) {
                sh.g = 1 / r.model.rTerm;
                sh.j = r.model.vTerm / r.model.rTerm;
            }
            net.shunts.push_back(sh);
        }
    }
    net.ports.push_back(circ[0].driverNode + offset[0]);
    if (m.differential) net.ports.push_back(circ[1].driverNode + offset[1]);
    net.ports.push_back(rxP.node + offset[0]);
    if (m.differential) net.ports.push_back(circ[1].receivers[static_cast<size_t>(rx[1])].node + offset[1]);
    if (m.differential && m.coupled.empty())
        m.notes.push_back("No P / N sections run side by side: the legs are analysed uncoupled");
    return m;
}

// ---- terminated response -----------------------------------------------------------------------------------------------

SParams channelSParams(const ChannelModel& m, const std::vector<double>& freq, double z0, const TouchstoneData* cascade) {
    SParams sp;
    sp.ports = static_cast<int>(m.network.ports.size());
    sp.z0 = z0;
    sp.freq = freq;
    sp.s = m.network.sParameters(freq, z0);
    if (cascade && cascade->sp.ports == sp.ports && sp.ports % 2 == 0) {
        SParams t = interpolateSParams(cascade->sp, freq);
        // Renormalise the imported block to z0 when its reference differs.
        if (std::fabs(t.z0 - z0) > 1e-9) {
            const int P = t.ports;
            for (auto& s : t.s) {
                // S → Z (ref t.z0) → S (ref z0): Z = z0'(I + S)(I − S)⁻¹.
                CMat a = CMat::identity(P) - s;
                if (!invertInPlace(a)) continue;
                CMat zm = (CMat::identity(P) + s) * a;
                for (auto& v : zm.a) v *= t.z0;
                CMat l = zm, r = zm;
                for (int i = 0; i < P; ++i) {
                    l(i, i) += z0;
                    r(i, i) -= z0;
                }
                if (!invertInPlace(l)) continue;
                s = r * l;
            }
        }
        for (size_t k = 0; k < sp.s.size(); ++k) sp.s[k] = cascadeStar(sp.s[k], t.s[k]);
    }
    return sp;
}

namespace {

/// Driver (Thévenin at the pad) and receiver (admittance at the pad) of one leg at f.
PortTermination driverTermination(const ChannelModel& m, const ChannelDrive& d, double f, double v, double z0) {
    const double w = 2 * kPi * f;
    if (d.idealDriver) return PortTermination::source(v, d.sourceOhms, z0);
    const DriverModel& dm = m.driver;
    const double rDie = std::max(0.1, dm.rOut + dm.rPkg);
    // Source behind R_out with C_comp at the die, then the series resistor, then C_pkg at the pad (Norton steps).
    cplx y1 = 1.0 / rDie + cplx(0, w * dm.cComp);
    cplx z1 = 1.0 / y1, v1 = (v / rDie) * z1;
    cplx z2 = z1 + m.seriesR;
    cplx y3 = 1.0 / z2 + cplx(0, w * dm.cPkg);
    cplx zt = 1.0 / y3, vt = (v1 / z2) * zt;
    return PortTermination::source(vt, zt, z0);
}

cplx receiverAdmittance(const ChannelModel& m, const ChannelDrive& d, double f) {
    if (d.idealDriver) return 1.0 / d.termOhms;
    const DriverModel& r = m.receiver;
    return cplx(r.rTerm > 0 ? 1 / r.rTerm : 0, 2 * kPi * f * (r.cIn + r.cPkg));
}

}  // namespace

std::vector<cplx> channelTransfer(const ChannelModel& m, const std::vector<double>& freq, double z0, const ChannelDrive& drive,
                                  const TouchstoneData* cascade) {
    std::vector<cplx> h(freq.size(), cplx(0, 0));
    const SParams sp = channelSParams(m, freq, z0, cascade);
    const int P = sp.ports;
    if (P != 2 && P != 4) return h;
    for (size_t k = 0; k < freq.size(); ++k) {
        const double f = freq[k];
        std::vector<PortTermination> t;
        if (P == 2) {
            t = {driverTermination(m, drive, f, 1.0, z0), PortTermination::load(receiverAdmittance(m, drive, f), z0)};
            h[k] = terminatedVoltages(sp.s[k], z0, t)[1];
        } else {
            const PortTermination rx = PortTermination::load(receiverAdmittance(m, drive, f), z0);
            t = {driverTermination(m, drive, f, 1.0, z0), driverTermination(m, drive, f, -1.0, z0), rx, rx};
            const auto v = terminatedVoltages(sp.s[k], z0, t);
            h[k] = v[2] - v[3];
        }
    }
    return h;
}

double channelDcLevel(const ChannelModel& m, double vP, double vN, const ChannelDrive& drive) {
    const ChannelNetwork& net = m.network;
    std::vector<ChannelNetwork::Shunt> extra;
    const bool diff = m.differential && net.ports.size() == 4;
    auto addDriver = [&](int node, double v) {
        const double r = drive.idealDriver ? drive.sourceOhms : std::max(0.1, m.driver.rOut + m.driver.rPkg) + m.seriesR;
        extra.push_back({node, 1 / r, 0, v / r});
    };
    auto addReceiver = [&](int node) {
        if (drive.idealDriver) extra.push_back({node, 1 / drive.termOhms, 0, 0});
        else if (m.receiver.rTerm > 0) extra.push_back({node, 1 / m.receiver.rTerm, 0, m.receiver.vTerm / m.receiver.rTerm});
    };
    if (net.ports.size() < 2) return 0;
    addDriver(net.ports[0], vP);
    if (diff) {
        addDriver(net.ports[1], vN);
        addReceiver(net.ports[2]);
        addReceiver(net.ports[3]);
        const auto v = net.dcVoltages(extra, {net.ports[2], net.ports[3]});
        return v[0] - v[1];
    }
    addReceiver(net.ports[1]);
    return net.dcVoltages(extra, {net.ports[1]})[0];
}

namespace {

double db(cplx v) { return 20 * std::log10(std::max(1e-12, std::abs(v))); }

Json numbers(const std::vector<double>& v) {
    Json a = Json::array();
    for (double x : v) a.push(std::isfinite(x) ? x : 0.0);
    return a;
}

/// Display curves: S21 / S11 / S22 (2-port) or SDD21 / SDD11 / SCD21 / SCC21 (4-port).
Json curvesJson(const SParams& sp, double* ilAt, double* rlAt, double fAt) {
    Json curves = Json::array();
    const bool four = sp.ports == 4;
    std::vector<std::string> names = four ? std::vector<std::string>{"SDD21", "SDD11", "SCD21", "SCC21"}
                                          : std::vector<std::string>{"S21", "S11", "S22"};
    std::vector<std::vector<double>> data(names.size());
    for (const auto& s : sp.s) {
        if (four) {
            const CMat mm = mixedMode(s);
            data[0].push_back(db(mm(1, 0)));
            data[1].push_back(db(mm(0, 0)));
            data[2].push_back(db(mm(3, 0)));
            data[3].push_back(db(mm(3, 2)));
        } else if (sp.ports == 2) {
            data[0].push_back(db(s(1, 0)));
            data[1].push_back(db(s(0, 0)));
            data[2].push_back(db(s(1, 1)));
        }
    }
    for (size_t i = 0; i < names.size(); ++i) {
        Json c = Json::object();
        c["name"] = names[i];
        c["db"] = numbers(data[i]);
        curves.push(c);
    }
    if (ilAt && rlAt && fAt > 0 && sp.freq.size() > 1 && !data[0].empty()) {
        size_t k = static_cast<size_t>(std::upper_bound(sp.freq.begin(), sp.freq.end(), fAt) - sp.freq.begin());
        k = std::clamp<size_t>(k, 1, sp.freq.size() - 1);
        const double t = std::clamp((fAt - sp.freq[k - 1]) / std::max(1e-30, sp.freq[k] - sp.freq[k - 1]), 0.0, 1.0);
        *ilAt = data[0][k - 1] + t * (data[0][k] - data[0][k - 1]);
        *rlAt = data[1][k - 1] + t * (data[1][k] - data[1][k - 1]);
    }
    return curves;
}

}  // namespace

Json channelJson(const Project& project, const ChannelOptions& opt, const ChannelDrive& drive, const EyeOptions* eyeOpt,
                 const TouchstoneData* cascade) {
    Json j = Json::object();
    ChannelModel m = extractChannel(project, opt);
    if (!m.error.empty()) {
        j["error"] = m.error;
        return j;
    }
    if (cascade && cascade->sp.ports != static_cast<int>(m.network.ports.size())) {
        m.notes.push_back("The imported Touchstone block has " + std::to_string(cascade->sp.ports) +
                          " ports; the channel needs " + std::to_string(m.network.ports.size()) + ": not cascaded");
        cascade = nullptr;
    } else if (cascade) {
        m.notes.push_back("Imported Touchstone block cascaded at the receiver end (outside its band: magnitude held, "
                          "phase extrapolated)");
    }
    const double z0 = opt.refOhms;
    j["net"] = m.netP;
    j["partner"] = m.netN;
    j["differential"] = m.differential;
    j["estimated"] = m.estimated;
    j["ports"] = static_cast<int>(m.network.ports.size());
    j["refOhms"] = z0;
    Json drv = Json::object();
    drv["ref"] = m.driverRef;
    drv["pin"] = m.driverPin;
    drv["model"] = drive.idealDriver ? std::string("ideal") : m.driver.name;
    drv["rOut"] = drive.idealDriver ? drive.sourceOhms : m.driver.rOut + m.driver.rPkg + m.seriesR;
    drv["riseTime"] = m.driver.riseTime;
    drv["swing"] = drive.idealDriver ? drive.swing : m.driver.vHigh;
    j["driver"] = drv;
    Json rcv = Json::object();
    rcv["ref"] = m.receiverRef;
    rcv["pin"] = m.receiverPin;
    rcv["model"] = drive.idealDriver ? std::string("ideal") : m.receiver.name;
    j["receiver"] = rcv;
    j["length"] = m.lengthP;
    j["lengthN"] = m.lengthN;
    j["delay"] = m.delayP;
    j["skew"] = m.differential ? m.delayP - m.delayN : 0.0;
    j["vias"] = m.vias;
    j["coupledLength"] = m.coupledLength;
    Json cs = Json::array();
    for (const auto& c : m.coupled) {
        Json x = Json::object();
        x["layer"] = c.layer;
        x["length"] = c.length;
        x["gap"] = c.gap;
        x["zEven"] = c.zEven;
        x["zOdd"] = c.zOdd;
        x["zDiff"] = c.zDiff;
        x["zComm"] = c.zComm;
        x["epsEven"] = c.epsEven;
        x["epsOdd"] = c.epsOdd;
        cs.push(x);
    }
    j["coupled"] = cs;

    // S-parameters on the display grid.
    std::vector<double> freq;
    for (int k = 0; k < opt.points; ++k) freq.push_back(opt.fMax * k / (opt.points - 1));
    const SParams sp = channelSParams(m, freq, z0, cascade);
    j["freq"] = numbers(freq);
    double il = 0, rl = 0;
    const double fNyq = eyeOpt ? eyeOpt->bitRate / 2 : 0;
    j["curves"] = curvesJson(sp, &il, &rl, fNyq);
    if (fNyq > 0) {
        Json ny = Json::object();
        ny["f"] = fNyq;
        ny["il"] = il;
        ny["rl"] = rl;
        j["nyquist"] = ny;
    }

    // Step response, lossy and lossless.
    const double swing = drive.idealDriver ? drive.swing : m.driver.vHigh;
    double rise = drive.riseTime > 0 ? drive.riseTime : drive.idealDriver ? (eyeOpt ? 0.25 / eyeOpt->bitRate : 50e-12)
                                                                           : m.driver.riseTime;
    rise = std::max(1e-12, rise);
    const double delay = std::max(m.delayP, m.delayN);
    {
        ChannelOptions lo = opt;
        lo.loss.lossless = true;
        const ChannelModel ml = extractChannel(project, lo);
        const double tStop = std::max({20 * delay + 10 * rise, 2e-9, 6 * rise});
        const double dt = std::max(tStop / 4000, rise / 20);
        const bool diff = m.differential;
        const double vLow = channelDcLevel(m, 0, diff ? swing : 0, drive);
        auto lossy = stepResponse([&](const std::vector<double>& f) { return channelTransfer(m, f, z0, drive, cascade); }, rise, dt,
                                  tStop);
        auto ideal = stepResponse([&](const std::vector<double>& f) { return channelTransfer(ml, f, z0, drive, cascade); }, rise, dt,
                                  tStop);
        Json st = Json::object();
        std::vector<double> t, a, b;
        const size_t n = std::min(lossy.size(), ideal.size());
        const size_t step = std::max<size_t>(1, n / 500);
        for (size_t k = 0; k < n; k += step) {
            t.push_back(static_cast<double>(k) * dt);
            a.push_back(vLow + swing * lossy[k]);
            b.push_back(vLow + swing * ideal[k]);
        }
        st["time"] = numbers(t);
        st["lossy"] = numbers(a);
        st["lossless"] = numbers(b);
        j["step"] = st;
    }

    if (eyeOpt) {
        EyeOptions eo = *eyeOpt;
        if (!(eo.riseTime > 0)) eo.riseTime = drive.riseTime > 0 ? drive.riseTime : drive.idealDriver ? 0.25 / eo.bitRate : m.driver.riseTime;
        const double vMid = channelDcLevel(m, swing / 2, swing / 2, drive);
        const EyeResult e = simulateEye([&](const std::vector<double>& f) { return channelTransfer(m, f, z0, drive, cascade); },
                                        swing / 2, vMid, delay, eo);
        j["eye"] = eyeJson(e);
    }
    Json notes = Json::array();
    for (const auto& n : m.notes) notes.push(n);
    j["notes"] = notes;
    return j;
}

std::string channelTouchstone(const Project& project, const ChannelOptions& opt, std::string* error) {
    const ChannelModel m = extractChannel(project, opt);
    if (!m.error.empty()) {
        if (error) *error = m.error;
        return "";
    }
    std::vector<double> freq;
    for (int k = 0; k < opt.points; ++k) freq.push_back(opt.fMax * k / (opt.points - 1));
    const SParams sp = channelSParams(m, freq, opt.refOhms);
    std::string comment = "SiEDA channel " + m.netP + (m.differential ? " / " + m.netN : "") + ", " + m.driverRef + " → " +
                          m.receiverRef;
    if (m.differential) comment += "; ports 1 P near, 2 N near, 3 P far, 4 N far";
    return writeTouchstone(sp, comment);
}

}  // namespace sieda
