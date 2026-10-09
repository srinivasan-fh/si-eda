#include "sieda/FieldSolver.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

#include "sieda/Stackup.hpp"

namespace sieda {

namespace {
constexpr double kEps0 = 8.8541878128e-12, kC = 299792458.0, kMu0 = 1.25663706212e-6, kRhoCu = 1.72e-8, kPiF = 3.14159265358979;

/// Grid lines through every break, cells growing ×1.3 from each break up to `hmax`, symmetric within each interval.
std::vector<double> axis(std::vector<double> br, double hmin, double hmax) {
    std::sort(br.begin(), br.end());
    br.erase(std::unique(br.begin(), br.end(), [](double a, double b) { return b - a < 1e-9; }), br.end());
    std::vector<double> out{br[0]};
    for (size_t k = 1; k < br.size(); ++k) {
        const double a = br[k - 1], b = br[k], half = (b - a) / 2;
        std::vector<double> st;
        double sum = 0;
        for (double d = hmin; sum + d < half; d = std::min(d * 1.2, hmax)) st.push_back(d), sum += d;
        const double scale = st.empty() ? 1 : half / sum;
        double x = a;
        for (double d : st) out.push_back(x += d * scale);
        for (size_t i = st.size(); i-- > 1;) out.push_back(x += st[i] * scale);
        out.push_back(b);
    }
    return out;
}

struct Grid {
    std::vector<double> x, y;
    std::vector<double> gx, gy;  // edge conductances (i,j)-(i+1,j) and (i,j)-(i,j+1)
    std::vector<int> owner;      // -1 free, 0 boundary (0 V), 1 / 2 conductor
    size_t nx = 0, ny = 0;
    size_t at(size_t i, size_t j) const { return j * nx + i; }
};

struct Charges {
    double q1 = 0, q2 = 0;
    double rGeom = 0;  // ∮ρ²dl / (∮ρdl)² over the signal and the return surfaces, 1/mm (current crowding)
};

/// Charges (per ε0, per metre) on conductors 1 and 2 with conductor 1 at 1 V, everything else at 0 V. The surface charge
/// is the surface current of the TEM mode, so its spread gives the skin-effect resistance R = Rs · rGeom.
Charges charges(const Grid& g) {
    const size_t n = g.nx * g.ny;
    std::vector<double> v(n, 0), r(n, 0), z(n, 0), p(n, 0), q(n, 0), diag(n, 0);
    for (size_t k = 0; k < n; ++k) v[k] = g.owner[k] == 1 ? 1 : 0;
    // A·u over the 4-neighbour stencil (u at fixed nodes counts as given).
    auto apply = [&](const std::vector<double>& u, std::vector<double>& out, bool withDiag) {
        for (size_t j = 0; j < g.ny; ++j)
            for (size_t i = 0; i < g.nx; ++i) {
                const size_t k = g.at(i, j);
                double s = 0, d = 0;
                auto edge = [&](double c, size_t m) { s += c * (u[k] - u[m]); d += c; };
                if (i > 0) edge(g.gx[g.at(i - 1, j)], k - 1);
                if (i + 1 < g.nx) edge(g.gx[k], k + 1);
                if (j > 0) edge(g.gy[g.at(i, j - 1)], k - g.nx);
                if (j + 1 < g.ny) edge(g.gy[k], k + g.nx);
                out[k] = s;
                if (withDiag) diag[k] = d;
            }
    };
    apply(v, r, true);
    double rz = 0, r0 = 0;
    for (size_t k = 0; k < n; ++k) {
        r[k] = g.owner[k] < 0 ? -r[k] : 0;
        z[k] = r[k] / std::max(diag[k], 1e-300);
        p[k] = z[k];
        rz += r[k] * z[k];
        r0 += r[k] * r[k];
    }
    for (int it = 0; it < 20000 && rz > 0; ++it) {
        apply(p, q, false);
        double pq = 0;
        for (size_t k = 0; k < n; ++k) pq += g.owner[k] < 0 ? p[k] * q[k] : 0;
        const double alpha = rz / pq;
        double rz2 = 0, rr = 0;
        for (size_t k = 0; k < n; ++k) {
            if (g.owner[k] >= 0) continue;
            v[k] += alpha * p[k];
            r[k] -= alpha * q[k];
            z[k] = r[k] / diag[k];
            rz2 += r[k] * z[k];
            rr += r[k] * r[k];
        }
        if (rr <= 1e-24 * r0) break;
        for (size_t k = 0; k < n; ++k) p[k] = g.owner[k] < 0 ? z[k] + rz2 / rz * p[k] : 0;
        rz = rz2;
    }
    apply(v, q, false);  // net flux out of each node
    Charges c;
    for (size_t k = 0; k < n; ++k) (g.owner[k] == 1 ? c.q1 : g.owner[k] == 2 ? c.q2 : r0) += q[k];
    // Flux through each conductor face: signal faces (conductor 1) and return faces (planes, box, conductor 2).
    double s2 = 0;
    auto face = [&](size_t k, size_t m, double gk, double len) {
        const int a = g.owner[k], b = g.owner[m];
        if ((a == 1) != (b == 1) || ((a >= 0) != (b >= 0))) s2 += (gk * (v[k] - v[m])) * (gk * (v[k] - v[m])) / len;
    };
    for (size_t j = 0; j < g.ny; ++j)
        for (size_t i = 0; i < g.nx; ++i) {
            const size_t k = g.at(i, j);
            const double dyS = j > 0 ? g.y[j] - g.y[j - 1] : 0, dyN = j + 1 < g.ny ? g.y[j + 1] - g.y[j] : 0;
            const double dxW = i > 0 ? g.x[i] - g.x[i - 1] : 0, dxE = i + 1 < g.nx ? g.x[i + 1] - g.x[i] : 0;
            if (i + 1 < g.nx) face(k, k + 1, g.gx[k], (dyS + dyN) / 2);
            if (j + 1 < g.ny) face(k, k + g.nx, g.gy[k], (dxW + dxE) / 2);
        }
    c.rGeom = c.q1 > 0 ? s2 / (c.q1 * c.q1) : 0;
    return c;
}
}  // namespace

FieldResult solveField(const FieldGeometry& in) {
    FieldGeometry g = in;
    g.w = std::max(g.w, 1e-3), g.h = std::max(g.h, 1e-3), g.t = std::max(g.t, 0.0), g.er = std::max(g.er, 1.0);
    const bool pair = g.s > 0, strip = g.hTop > 0;
    const double mask = strip ? 0 : std::clamp(g.mask, 0.0, 1.0);
    const double y1 = g.h, y2 = g.h + g.t, top = strip ? y2 + g.hTop : y2 + 12 * (g.h + g.w);
    const double xa = pair ? g.s / 2 : -g.w / 2, xb = xa + g.w;   // conductor 1; conductor 2 mirrored (pair)
    const double span = std::max(xb, std::fabs(xa)), side = span + (strip ? 5 * (y2 + g.hTop) : 12 * (g.h + g.w));
    double hmin = std::min(g.w, g.h) / 100;
    if (g.t > 0) hmin = std::min(hmin, g.t / 4);
    if (pair) hmin = std::min(hmin, g.s / 30);
    if (strip) hmin = std::min(hmin, g.hTop / 100);
    if (mask > 0) hmin = std::min(hmin, mask / 4);
    std::vector<double> bx{-side, xa, xb, side}, by{0, y1, y2, top};
    if (pair) bx.insert(bx.end(), {-xa, -xb, 0.0});
    if (mask > 0) {  // the coating's surfaces are grid lines too
        by.insert(by.end(), {y1 + mask, y2 + mask});
        bx.insert(bx.end(), {xa - mask, xb + mask, -(xa - mask), -(xb + mask)});
    }
    Grid grid;
    grid.x = axis(bx, hmin, (g.h + g.w) / 3);
    grid.y = axis(by, hmin, (g.h + g.w) / 3);
    grid.nx = grid.x.size(), grid.ny = grid.y.size();
    const size_t n = grid.nx * grid.ny;
    grid.owner.assign(n, -1);
    const double tol = 1e-9;
    for (size_t j = 0; j < grid.ny; ++j)
        for (size_t i = 0; i < grid.nx; ++i) {
            const double x = grid.x[i], y = grid.y[j];
            int& o = grid.owner[grid.at(i, j)];
            if (i == 0 || j == 0 || i + 1 == grid.nx || j + 1 == grid.ny) o = 0;
            else if (y >= y1 - tol && y <= y2 + tol && x >= xa - tol && x <= xb + tol) o = 1;
            else if (pair && y >= y1 - tol && y <= y2 + tol && x >= -xb - tol && x <= -xa + tol) o = 2;
        }
    double rGeom = 0;
    auto solve = [&](bool air) {
        auto eps = [&](size_t i, size_t j) {  // cell (i,j); out of range = 0
            if (i + 1 >= grid.nx || j + 1 >= grid.ny) return 0.0;
            if (air) return 1.0;
            const double cx = (grid.x[i] + grid.x[i + 1]) / 2, cy = (grid.y[j] + grid.y[j + 1]) / 2;
            if (strip || cy < y1) return g.er;
            auto coats = [&](double a, double b) { return cx > a - mask && cx < b + mask && cy < y2 + mask; };
            return mask > 0 && (cy < y1 + mask || coats(xa, xb) || (pair && coats(-xb, -xa))) ? std::max(g.erMask, 1.0) : 1.0;
        };
        grid.gx.assign(n, 0), grid.gy.assign(n, 0);
        for (size_t j = 0; j < grid.ny; ++j)
            for (size_t i = 0; i < grid.nx; ++i) {
                const double dyS = j > 0 ? grid.y[j] - grid.y[j - 1] : 0, dyN = j + 1 < grid.ny ? grid.y[j + 1] - grid.y[j] : 0;
                const double dxW = i > 0 ? grid.x[i] - grid.x[i - 1] : 0, dxE = i + 1 < grid.nx ? grid.x[i + 1] - grid.x[i] : 0;
                if (i + 1 < grid.nx) grid.gx[grid.at(i, j)] = ((j > 0 ? eps(i, j - 1) * dyS : 0) + eps(i, j) * dyN) / 2 / dxE;
                if (j + 1 < grid.ny) grid.gy[grid.at(i, j)] = ((i > 0 ? eps(i - 1, j) * dxW : 0) + eps(i, j) * dxE) / 2 / dyN;
            }
        const Charges c = charges(grid);
        if (air) rGeom = c.rGeom;
        return std::pair<double, double>{kEps0 * c.q1, kEps0 * c.q2};
    };
    const auto [a, b] = solve(false);
    const auto [a0, b0] = solve(true);
    FieldResult r;
    r.nodes = int(n);
    const double l11 = a0 / (kC * kC * (a0 * a0 - b0 * b0));  // (μ0ε0·C0⁻¹)₁₁
    r.z0 = std::sqrt(l11 / a);
    r.eeff = l11 * a * kC * kC;
    r.delayPsPerMm = std::sqrt(l11 * a) * 1e9;
    r.lNhPerMm = l11 * 1e6;
    r.cPfPerMm = a * 1e9;
    r.rGeom = rGeom * 1e3;
    r.rdc = g.t > 0 ? kRhoCu / (g.w * g.t * 1e-6) : 0;
    if (pair) {
        r.zodd = 1 / (kC * std::sqrt((a - b) * (a0 - b0)));
        r.zeven = 1 / (kC * std::sqrt((a + b) * (a0 + b0)));
        r.zdiff = 2 * r.zodd, r.zcommon = r.zeven / 2;
        r.kb = (-b0 / a0 - b / a) / 4;
        r.kf = (-b / a + b0 / a0) / 2;
    }
    return r;
}

LineLoss lineLoss(const FieldGeometry& g, const FieldResult& r, double f) {
    LineLoss l;
    l.f = f = std::max(f, 0.0);
    if (!(r.z0 > 0)) return l;
    double rac = 0;
    if (f > 0) {
        const double rs = std::sqrt(kPiF * f * kMu0 * kRhoCu), delta = std::sqrt(kRhoCu / (kPiF * f * kMu0)) * 1e6;  // µm
        const double rough = 1 + 2 / kPiF * std::atan(1.4 * std::pow(std::max(g.roughness, 0.0) / delta, 2));
        rac = rs * rough * r.rGeom;
    }
    const double rTot = std::hypot(r.rdc, rac);
    const double er = std::max(g.er, 1.0), q = er > 1 + 1e-9 ? er * (r.eeff - 1) / (r.eeff * (er - 1)) : 1;
    const double gS = 2 * kPiF * f * std::max(g.tanD, 0.0) * r.cPfPerMm * 1e-9 * std::clamp(q, 0.0, 1.0);  // S/m
    const double dbPerIn = 8.685889638 * 0.0254;
    l.rOhmPerMm = rTot * 1e-3;
    l.conductorDbPerIn = rTot / (2 * r.z0) * dbPerIn;
    l.dielectricDbPerIn = gS * r.z0 / 2 * dbPerIn;
    l.totalDbPerIn = l.conductorDbPerIn + l.dielectricDbPerIn;
    return l;
}

double fieldSolvedWidth(const BoardSettings& s, int layer, double ohms, double gap) {
    auto z = [&](double w) {
        const FieldResult r = solveField(trackGeometry(s, layer, w, gap));
        return gap > 0 ? r.zdiff : r.z0;
    };
    // Z falls as the track widens: bracket around the closed-form width, then bisect on log(w).
    const double w0 = std::clamp(gap > 0 ? differentialPairGeometry(s, layer, ohms).first : widthForImpedance(s, layer, ohms), 0.05, 5.0);
    double lo = w0 / 1.5, hi = w0 * 1.5, zLo = z(lo), zHi = z(hi);
    while (lo > 0.02 && zLo < ohms) zLo = z(lo /= 2);
    while (hi < 10 && zHi > ohms) zHi = z(hi *= 2);
    if (zLo < ohms || zHi > ohms) return 0;
    // Regula falsi (Illinois) on ln Z against ln w — nearly linear, so a few solves reach 0.1 %.
    double fLo = std::log(zLo / ohms), fHi = std::log(zHi / ohms), w = std::sqrt(lo * hi);
    for (int i = 0, side = 0; i < 30 && hi / lo > 1.0005; ++i) {
        w = std::exp(std::log(lo) + fLo / (fLo - fHi) * std::log(hi / lo));
        const double f = std::log(z(w) / ohms);
        if (std::fabs(f) < 1e-3) break;
        if (f > 0) lo = w, fLo = f, fHi *= side == 1 ? 0.5 : 1, side = 1;
        else hi = w, fHi = f, fLo *= side == -1 ? 0.5 : 1, side = -1;
    }
    return w;
}

FieldGeometry trackGeometry(const BoardSettings& s, int layer, double w, double gap) {
    FieldGeometry g;
    g.w = w, g.s = gap, g.t = copperThickness(s), g.er = boardLaminate(s).er, g.tanD = boardLaminate(s).lossTangent;
    if (isStriplineLayer(s, layer)) g.h = dielectricBelow(s, layer), g.hTop = dielectricBelow(s, layer - 1);
    else g.h = impedanceReferenceHeight(s, layer), g.mask = 0.02;
    return g;
}

Json fieldResultJson(const FieldGeometry& g, const FieldResult& r) {
    Json j = Json::object();
    j["geometry"] = Json::object();
    j["geometry"]["w"] = g.w, j["geometry"]["t"] = g.t, j["geometry"]["h"] = g.h, j["geometry"]["hTop"] = g.hTop;
    j["geometry"]["s"] = g.s, j["geometry"]["er"] = g.er;
    if (g.mask > 0 && g.hTop <= 0) j["geometry"]["mask"] = g.mask, j["geometry"]["erMask"] = g.erMask;
    j["geometry"]["tanD"] = g.tanD, j["geometry"]["roughness"] = g.roughness;
    j["geometry"]["kind"] = g.hTop > 0 ? "stripline" : "microstrip";
    j["z0"] = r.z0, j["eeff"] = r.eeff, j["delayPsPerMm"] = r.delayPsPerMm;
    j["lNhPerMm"] = r.lNhPerMm, j["cPfPerMm"] = r.cPfPerMm, j["nodes"] = r.nodes;
    Json loss = Json::array();
    for (double ghz : {0.1, 1.0, 2.5, 5.0, 10.0, 25.0}) {
        const LineLoss l = lineLoss(g, r, ghz * 1e9);
        Json e = Json::object();
        e["ghz"] = ghz, e["rOhmPerMm"] = l.rOhmPerMm, e["conductorDbPerIn"] = l.conductorDbPerIn;
        e["dielectricDbPerIn"] = l.dielectricDbPerIn, e["totalDbPerIn"] = l.totalDbPerIn;
        loss.push(e);
    }
    j["loss"] = loss;
    if (g.s > 0) {
        j["zodd"] = r.zodd, j["zeven"] = r.zeven, j["zdiff"] = r.zdiff, j["zcommon"] = r.zcommon;
        j["kb"] = r.kb, j["kf"] = r.kf;
    }
    return j;
}

}  // namespace sieda
