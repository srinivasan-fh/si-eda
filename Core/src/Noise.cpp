#include "sieda/Noise.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <map>

#include "SimulatorInternal.hpp"
#include "sieda/Geometry.hpp"
#include "sieda/Units.hpp"

namespace sieda {

using namespace simdetail;

namespace {
/// A noise source: its current (or, for an op-amp's input voltage noise, the equation residual) enters the MNA rows
/// `inject` with their coefficients; PSD(f) = white + flicker / f.
struct Source {
    int componentId = -1;
    const char* kind = "";
    std::vector<std::pair<int, double>> inject;
    double white = 0, flicker = 0;
};
}  // namespace

NoiseResult Simulator::noise(const NoiseOptions& o) {
    using Cplx = std::complex<double>;
    NoiseResult res;
    if (!(o.fStart > 0) || !(o.fStop > o.fStart) || !std::isfinite(o.fStop)) {
        res.error = "Noise analysis needs a start frequency above 0 and a stop frequency above it.";
        return res;
    }
    if (o.pointsPerDecade < 1 || o.pointsPerDecade > 1000) {
        res.error = "Noise analysis needs 1 to 1000 points per decade.";
        return res;
    }
    const double decades = std::log10(o.fStop / o.fStart);
    const double intervals = std::max(1.0, std::ceil(decades * o.pointsPerDecade - 1e-9));
    if (intervals > 20000) {
        res.error = "Noise analysis is limited to 20 000 frequency points: narrow the range or use fewer points per decade.";
        return res;
    }
    if (!(o.temperature > 0) || !std::isfinite(o.temperature)) {
        res.error = "Noise analysis needs a positive temperature.";
        return res;
    }
    const auto& nets = sch_.nets();
    if (o.outputNet < 0 || static_cast<size_t>(o.outputNet) >= nets.size() ||
        (o.referenceNet >= 0 && static_cast<size_t>(o.referenceNet) >= nets.size())) {
        res.error = "Choose the output net.";
        return res;
    }
    if (!build(res.error)) return res;
    std::vector<double> x(static_cast<size_t>(unknowns_), 0.0);
    int iters = 0;
    if (!operatingPoint(x, iters)) {
        res.error = "Noise analysis needs the DC operating point, which did not converge (check for floating nodes or "
                    "unrealistic values).";
        return res;
    }
    const int outNode = netToNode_[static_cast<size_t>(o.outputNet)];
    const int refNode = o.referenceNet >= 0 ? netToNode_[static_cast<size_t>(o.referenceNet)] : -1;
    if (outNode == refNode) {
        res.error = "The output and the reference are the same node: choose another output.";
        return res;
    }
    noLimit_ = true;
    stamp(0, 0, x, 0.0, 1.0);
    noLimit_ = false;
    const std::vector<double> G = A_;
    const int n = unknowns_;

    // Noise sources at the operating point.
    const double kT = spicedev::kBoltzmann * o.temperature, q = spicedev::kCharge;
    std::vector<Source> sources;
    auto current = [&](int id, const char* kind, int a, int b, double white, double flicker) {
        Source s;
        s.componentId = id;
        s.kind = kind;
        if (a >= 0) s.inject.push_back({a, 1.0});
        if (b >= 0) s.inject.push_back({b, -1.0});
        if (!s.inject.empty() && (white > 0 || flicker > 0) && std::isfinite(white) && std::isfinite(flicker)) {
            s.white = white;
            s.flicker = flicker;
            sources.push_back(s);
        }
    };
    auto opAmpNoise = [&](int componentId, double& en, double& in, double& fnc) {
        en = in = fnc = 0;
        const Component* c = sch_.find(componentId);
        if (!c) return;
        opAmpValueParam(c->value, "EN", en);
        opAmpValueParam(c->value, "IN", in);
        opAmpValueParam(c->value, "FNC", fnc);
        en = std::max(en, 0.0);
        in = std::max(in, 0.0);
        fnc = std::max(fnc, 0.0);
    };
    for (const auto& e : elements_) {
        std::array<double, 3> v{{nodeV(x, e.n[0]), nodeV(x, e.n[1]), nodeV(x, e.n[2])}};
        switch (e.type) {
            case ElemType::Resistor:
                if (!e.isSwitch && e.value != 0) current(e.componentId, "thermal", e.n[0], e.n[1], 4 * kT / std::fabs(e.value), 0);
                break;
            case ElemType::Diode: {
                const double i = deviceCurrents(e, v)[0];
                current(e.componentId, "shot", e.n[0], e.n[1], 2 * q * std::fabs(i), 0);
                break;
            }
            case ElemType::NPN: {  // B, C, E
                const auto i = deviceCurrents(e, v);
                current(e.componentId, "shot", e.n[1], e.n[2], 2 * q * std::fabs(i[1]), 0);
                current(e.componentId, "shot", e.n[0], e.n[2], 2 * q * std::fabs(i[0]), 0);
                break;
            }
            case ElemType::NMOS: {  // G, D, S
                auto up = v, dn = v;
                up[0] += 1e-6;
                dn[0] -= 1e-6;
                const double gm = (deviceCurrents(e, up)[1] - deviceCurrents(e, dn)[1]) / 2e-6;
                current(e.componentId, "thermal", e.n[1], e.n[2], 8.0 / 3.0 * kT * std::fabs(gm), 0);
                break;
            }
            case ElemType::Device: {
                std::array<double, 4> dv{};
                for (int k = 0; k < e.dev->terminals; ++k) dv[static_cast<size_t>(k)] = nodeV(x, e.dn[static_cast<size_t>(k)]);
                std::vector<spicedev::NoiseTerm> terms;
                e.dev->noise(dv.data(), terms);
                for (const auto& t : terms) {
                    const int a = e.dn[static_cast<size_t>(t.a)], b = e.dn[static_cast<size_t>(t.b)];
                    // A model's thermal/shot term in kT units is at 300.15 K: rescale to the analysis temperature.
                    const double white = std::string(t.label) == "thermal" ? t.white * o.temperature / spicedev::kNominalTemp : t.white;
                    current(e.componentId, t.label, a, b, white, 0);
                    current(e.componentId, "flicker", a, b, 0, t.flicker);
                }
                break;
            }
            case ElemType::OpAmp: {  // the single-pole model: v_n enters through the open-loop gain at the bias point
                double en, in, fnc;
                opAmpNoise(e.componentId, en, in, fnc);
                const double th = std::tanh(kOpAmpGain * (v[0] - v[1]) / e.vsat);
                const double slope = kOpAmpGain * (1.0 - th * th);
                if (en > 0) {
                    Source s;
                    s.componentId = e.componentId;
                    s.kind = "voltage";
                    s.inject = {{e.branch, slope}};
                    s.white = en * en;
                    s.flicker = en * en * fnc;
                    sources.push_back(s);
                }
                current(e.componentId, "current", e.n[0], -1, in * in, in * in * fnc);
                current(e.componentId, "current", e.n[1], -1, in * in, in * in * fnc);
                break;
            }
            case ElemType::Ctrl: {
                if (e.ctrl != CtrlKind::TanhStage) break;  // the macromodel's input stage carries the op-amp's noise
                double en, in, fnc;
                opAmpNoise(e.componentId, en, in, fnc);
                const auto& p = e.pairs[0];
                const int inp = p[0] >= 0 ? e.refs[static_cast<size_t>(p[0])] : -1;
                const int inm = p[1] >= 0 ? e.refs[static_cast<size_t>(p[1])] : -1;
                const double vd = nodeV(x, inp) - nodeV(x, inm);
                const double th = std::tanh(e.coeffs[0] * vd / e.coeffs[1]);
                const double g = e.coeffs[0] * (1 - th * th);
                if (en > 0 && e.n[1] >= 0) {
                    Source s;
                    s.componentId = e.componentId;
                    s.kind = "voltage";
                    s.inject = {{e.n[1], g}};
                    s.white = en * en;
                    s.flicker = en * en * fnc;
                    sources.push_back(s);
                }
                current(e.componentId, "current", inp, -1, in * in, in * in * fnc);
                current(e.componentId, "current", inm, -1, in * in, in * in * fnc);
                break;
            }
            default: break;
        }
    }

    // Input source (as the AC stimulus is chosen).
    int input = -1;
    auto isSource = [](const Element& e) {
        return !e.internal && (e.type == ElemType::VSource || e.type == ElemType::ISource);
    };
    if (o.sourceId >= 0) {
        for (size_t i = 0; i < elements_.size() && input < 0; ++i)
            if (elements_[i].componentId == o.sourceId && isSource(elements_[i])) input = static_cast<int>(i);
        if (input < 0) {
            res.error = "The noise input must be an independent voltage or current source.";
            return res;
        }
    } else {
        for (size_t i = 0; i < elements_.size() && input < 0; ++i)
            if (isSource(elements_[i]) && elements_[i].source.hasAc && elements_[i].source.acMagnitude != 0)
                input = static_cast<int>(i);
        for (size_t i = 0; i < elements_.size() && input < 0; ++i) {
            const Element& e = elements_[i];
            const Component* c = sch_.find(e.componentId);
            if (isSource(e) && (e.source.kind == SourceSpec::Kind::Sine || (c && c->kind == ComponentKind::ACSource)))
                input = static_cast<int>(i);
        }
    }
    if (input >= 0) {
        res.inputSource = elements_[static_cast<size_t>(input)].componentId;
        res.inputIsCurrent = elements_[static_cast<size_t>(input)].type == ElemType::ISource;
    }

    // Sweep: one adjoint solve per frequency carries every source to the output.
    const auto points = static_cast<size_t>(intervals) + 1;
    std::vector<std::vector<double>> perSource(sources.size());
    std::vector<double> total, totalIn;
    std::vector<Cplx> M, MT(static_cast<size_t>(n) * static_cast<size_t>(n)), y;
    for (size_t i = 0; i < points; ++i) {
        const double f = i + 1 == points ? o.fStop : o.fStart * std::pow(10.0, static_cast<double>(i) / o.pointsPerDecade);
        acMatrix(f, G, x, M);
        for (int r = 0; r < n; ++r)
            for (int c = 0; c < n; ++c)
                MT[static_cast<size_t>(r * n + c)] = M[static_cast<size_t>(c * n + r)];
        y.assign(static_cast<size_t>(n), Cplx(0.0, 0.0));
        if (outNode >= 0) y[static_cast<size_t>(outNode)] += 1.0;
        if (refNode >= 0) y[static_cast<size_t>(refNode)] -= 1.0;
        if (!luSolveComplex(MT, y, n)) {
            res.error = "Noise analysis: the circuit matrix is singular at " + formatEngineeringValue(f, "Hz") +
                        " (a floating node or a loop of voltage sources).";
            return res;
        }
        double s = 0;
        for (size_t k = 0; k < sources.size(); ++k) {
            Cplx t(0.0, 0.0);
            for (const auto& [row, coeff] : sources[k].inject) t += coeff * y[static_cast<size_t>(row)];
            const double sk = std::norm(t) * (sources[k].white + sources[k].flicker / f);
            perSource[k].push_back(sk);
            s += sk;
        }
        res.frequency.push_back(f);
        res.outputDensity.push_back(std::sqrt(s));
        total.push_back(s);
        if (input >= 0) {
            const Element& in = elements_[static_cast<size_t>(input)];
            Cplx h = in.type == ElemType::VSource
                         ? y[static_cast<size_t>(in.branch)]
                         : (in.n[0] >= 0 ? y[static_cast<size_t>(in.n[0])] : Cplx(0.0, 0.0)) -
                               (in.n[1] >= 0 ? y[static_cast<size_t>(in.n[1])] : Cplx(0.0, 0.0));
            const double gain = std::abs(h);
            res.gain.push_back(gain);
            const double sin = gain > 0 ? s / (gain * gain) : NAN;
            res.inputDensity.push_back(std::sqrt(sin));
            totalIn.push_back(sin);
        }
    }
    auto integrate = [&](const std::vector<double>& psd) {
        double acc = 0;
        for (size_t i = 1; i < psd.size() && i < res.frequency.size(); ++i)
            if (std::isfinite(psd[i]) && std::isfinite(psd[i - 1]))
                acc += 0.5 * (psd[i] + psd[i - 1]) * (res.frequency[i] - res.frequency[i - 1]);
        return acc;
    };
    res.outputRms = std::sqrt(integrate(total));
    if (input >= 0) res.inputRms = std::sqrt(integrate(totalIn));
    std::map<std::pair<int, std::string>, double> byPart;
    for (size_t k = 0; k < sources.size(); ++k) byPart[{sources[k].componentId, sources[k].kind}] += integrate(perSource[k]);
    for (const auto& [key, v2] : byPart) res.contributions.push_back({key.first, key.second, std::sqrt(v2)});
    std::stable_sort(res.contributions.begin(), res.contributions.end(),
                     [](const NoiseContribution& a, const NoiseContribution& b) { return a.rms > b.rms; });
    res.ok = true;
    return res;
}

NoiseResult noiseAnalysis(const Schematic& schematic, const NoiseOptions& options) {
    Simulator sim(schematic);
    return sim.noise(options);
}

// ------------------------------------------------------------------ JSON

namespace {
double optNumber(const Json& j, double def) {
    if (j.isNumber()) return j.asNumber();
    if (j.isString())
        if (auto v = parseEngineeringValue(j.asString())) return *v;
    return def;
}
int netNamed(const Schematic& s, const Json& j) {
    const auto& nets = s.nets();
    if (j.isNumber()) {
        int i = j.asInt(-1);
        return i >= 0 && static_cast<size_t>(i) < nets.size() ? i : -1;
    }
    if (!j.isString()) return -1;
    const std::string name = j.asString();
    for (const auto& n : nets)
        if (n.name == name) return n.index;
    auto lower = [](std::string v) {
        for (char& c : v) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return v;
    };
    for (const auto& n : nets)
        if (lower(n.name) == lower(name)) return n.index;
    return -1;
}
Json errorJson(const std::string& m) {
    Json j = Json::object();
    j["ok"] = false;
    j["error"] = m;
    return j;
}
Json numbersJson(const std::vector<double>& v) {
    Json a = Json::array();
    for (double d : v) a.push(d);
    return a;
}
}  // namespace

Json simulateNoiseJson(const Schematic& s, const Json& options) {
    NoiseOptions o;
    o.outputNet = netNamed(s, options.get("output"));
    if (o.outputNet < 0) return errorJson("Choose the output net.");
    const Json& ref = options.get("reference");
    if (!ref.isNull() && !(ref.isString() && ref.asString().empty())) {
        o.referenceNet = netNamed(s, ref);
        if (o.referenceNet < 0) return errorJson("Unknown reference net '" + ref.asString("") + "'.");
    }
    const Json& src = options.get("source");
    if (src.isString() && !src.asString().empty()) {
        const Component* c = s.findByRef(src.asString());
        if (!c) return errorJson("Unknown noise input source '" + src.asString() + "'.");
        o.sourceId = c->id;
    }
    o.fStart = optNumber(options.get("start"), o.fStart);
    o.fStop = optNumber(options.get("stop"), o.fStop);
    o.pointsPerDecade = static_cast<int>(optNumber(options.get("pointsPerDecade"), o.pointsPerDecade));
    if (options.has("temperature")) o.temperature = optNumber(options.get("temperature"), 27.0) + 273.15;
    NoiseResult r = noiseAnalysis(s, o);
    if (!r.ok) return errorJson(r.error);
    Json root = Json::object();
    root["ok"] = true;
    root["error"] = "";
    root["output"] = s.nets()[static_cast<size_t>(o.outputNet)].name;
    root["reference"] = o.referenceNet >= 0 ? s.nets()[static_cast<size_t>(o.referenceNet)].name : std::string();
    root["frequency"] = numbersJson(r.frequency);
    root["outputDensity"] = numbersJson(r.outputDensity);
    root["outputRms"] = r.outputRms;
    const Component* in = r.inputSource >= 0 ? s.find(r.inputSource) : nullptr;
    root["inputSource"] = in ? in->ref : std::string();
    root["inputUnit"] = r.inputIsCurrent ? "A" : "V";
    if (in) {
        root["inputDensity"] = numbersJson(r.inputDensity);
        root["gain"] = numbersJson(r.gain);
        root["inputRms"] = r.inputRms;
    } else {
        root["inputDensity"] = Json();
        root["gain"] = Json();
        root["inputRms"] = Json();
    }
    Json parts = Json::array();
    for (size_t i = 0; i < r.contributions.size() && i < 30; ++i) {
        const auto& c = r.contributions[i];
        Json j = Json::object();
        const Component* comp = s.find(c.componentId);
        j["ref"] = comp ? comp->ref : std::string();
        j["kind"] = c.kind;
        j["rms"] = c.rms;
        parts.push(j);
    }
    root["contributions"] = parts;
    return root;
}

}  // namespace sieda
