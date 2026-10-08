#include "sieda/Analysis.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <complex>
#include <limits>

#include "sieda/Geometry.hpp"
#include "sieda/Units.hpp"

namespace sieda {

// ------------------------------------------------------------------ tolerances

double componentTolerance(const Component& c, const ToleranceDefaults& defaults, bool* fromValue) {
    if (fromValue) *fromValue = false;
    double def = 0;
    switch (c.kind) {
        case ComponentKind::Resistor: def = defaults.resistor; break;
        case ComponentKind::Capacitor: def = defaults.capacitor; break;
        case ComponentKind::Inductor: def = defaults.inductor; break;
        default: return 0;
    }
    // A percentage token after the value: "10k 1%", "100n ±5%", "4u7 20 %".
    const std::string& v = c.value;
    size_t pct = v.find('%');
    if (pct != std::string::npos) {
        size_t end = pct;
        while (end > 0 && v[end - 1] == ' ') --end;
        size_t begin = end;
        while (begin > 0 && (std::isdigit(static_cast<unsigned char>(v[begin - 1])) || v[begin - 1] == '.')) --begin;
        if (begin < end && begin > 0) {  // a value must come first
            try {
                double t = std::stod(v.substr(begin, end - begin)) / 100.0;
                if (t >= 0 && t < 1) {
                    if (fromValue) *fromValue = true;
                    return t;
                }
            } catch (...) {
            }
        }
    }
    return def;
}

uint64_t SeededRandom::next() {
    uint64_t z = (state_ += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

double SeededRandom::uniform() { return static_cast<double>(next() >> 11) * (1.0 / 9007199254740992.0); }

double SeededRandom::normal() {
    if (haveSpare_) {
        haveSpare_ = false;
        return spare_;
    }
    double u1 = 0;
    while (u1 <= 0) u1 = uniform();
    double u2 = uniform();
    double r = std::sqrt(-2.0 * std::log(u1));
    spare_ = r * std::sin(2 * kPi * u2);
    haveSpare_ = true;
    return r * std::cos(2 * kPi * u2);
}

std::optional<double> measure(const Schematic& schematic, const Measurement& m, const std::map<int, double>& scale,
                              std::string& error) {
    if (m.net < 0 || static_cast<size_t>(m.net) >= schematic.nets().size()) {
        error = "Choose the net to measure.";
        return std::nullopt;
    }
    Simulator sim(schematic);
    sim.setValueScale(scale);
    if (m.kind == Measurement::Kind::DcVoltage) {
        DcResult r = sim.dcOperatingPoint();
        if (!r.converged) {
            error = r.error;
            return std::nullopt;
        }
        return r.netVoltages[static_cast<size_t>(m.net)];
    }
    AcOptions o = m.ac;
    o.measureNets = {m.net};
    AcResult r = sim.ac(o);
    if (!r.ok) {
        error = r.error;
        return std::nullopt;
    }
    const AcMetrics& mt = r.metrics[m.net];
    double v = m.kind == Measurement::Kind::AcGainDb ? mt.lowFreqDb
               : m.kind == Measurement::Kind::AcF3db  ? mt.f3dbHz
                                                      : mt.peakHz;
    if (!std::isfinite(v)) {
        error = "The response has no -3 dB point inside the frequency sweep.";
        return std::nullopt;
    }
    return v;
}

ToleranceResult toleranceAnalysis(const Schematic& schematic, const Measurement& m, const ToleranceOptions& options) {
    ToleranceResult res;
    if (options.runs < 0 || options.runs > 10000) {
        res.error = "Monte Carlo analysis takes 0 to 10 000 runs.";
        return res;
    }
    for (const auto& c : schematic.components()) {
        bool fromValue = false;
        double t = componentTolerance(c, options.defaults, &fromValue);
        if (t > 0) res.parts.push_back({c.id, t, fromValue, 0.0});
    }
    std::string err;
    auto nominal = measure(schematic, m, {}, err);
    if (!nominal) {
        res.error = err;
        return res;
    }
    res.nominal = *nominal;

    SeededRandom rng(options.seed);
    for (int run = 0; run < options.runs; ++run) {
        if (simulationStopRequested()) {
            res.error = kSimulationStopped;
            return res;
        }
        std::map<int, double> scale;
        for (const auto& p : res.parts) {
            double dev;
            if (options.gaussian) dev = std::clamp(rng.normal() / 3.0, -1.0, 1.0);
            else dev = 2.0 * rng.uniform() - 1.0;
            scale[p.componentId] = 1.0 + p.tolerance * dev;
        }
        if (auto v = measure(schematic, m, scale, err)) res.samples.push_back(*v);
        else ++res.failedRuns;
    }
    if (!res.samples.empty()) {
        res.min = *std::min_element(res.samples.begin(), res.samples.end());
        res.max = *std::max_element(res.samples.begin(), res.samples.end());
        double sum = 0;
        for (double v : res.samples) sum += v;
        res.mean = sum / static_cast<double>(res.samples.size());
        double ss = 0;
        for (double v : res.samples) ss += (v - res.mean) * (v - res.mean);
        res.sigma = res.samples.size() > 1 ? std::sqrt(ss / static_cast<double>(res.samples.size() - 1)) : 0.0;
    }

    if (options.worstCase) {
        // Sensitivity of each part at its +tolerance end, then every part at the end that moves the result up
        // (maximum) or down (minimum). Exact for monotonic responses, which component values almost always give.
        const double eps = 1e-12 * std::max(1.0, std::fabs(res.nominal));
        for (auto& p : res.parts) {
            auto v = measure(schematic, m, {{p.componentId, 1.0 + p.tolerance}}, err);
            if (!v) {
                res.error = "Worst-case analysis failed: " + err;
                return res;
            }
            p.sensitivity = *v - res.nominal;
        }
        for (const auto& p : res.parts) {
            double dir = std::fabs(p.sensitivity) <= eps ? 0.0 : (p.sensitivity > 0 ? 1.0 : -1.0);
            res.worstMaxScale[p.componentId] = 1.0 + dir * p.tolerance;
            res.worstMinScale[p.componentId] = 1.0 - dir * p.tolerance;
        }
        auto hi = measure(schematic, m, res.worstMaxScale, err);
        auto lo = hi ? measure(schematic, m, res.worstMinScale, err) : std::nullopt;
        if (!hi || !lo) {
            res.error = "Worst-case analysis failed: " + err;
            return res;
        }
        res.hasWorstCase = true;
        res.worstMax = std::max(*hi, *lo);
        res.worstMin = std::min(*hi, *lo);
        if (*hi < *lo) std::swap(res.worstMaxScale, res.worstMinScale);
    }
    if (options.runs > 0 && res.samples.empty()) {
        res.error = "Every Monte Carlo run failed: " + err;
        return res;
    }
    res.ok = true;
    return res;
}

// ------------------------------------------------------------------ parameter sweep

bool ParamSweepRun::ok() const { return dc.converged || ac.ok || transient.ok; }

std::string ParamSweepRun::error() const {
    if (!dc.error.empty()) return dc.error;
    if (!ac.error.empty()) return ac.error;
    return transient.error;
}

std::vector<ParamSweepRun> parameterSweep(const Schematic& schematic, const ParamSweepOptions& options,
                                          std::string& error) {
    std::vector<ParamSweepRun> runs;
    const Component* c = schematic.find(options.componentId);
    if (!c) {
        error = "Choose the component to sweep.";
        return runs;
    }
    switch (c->kind) {
        case ComponentKind::Ground:
        case ComponentKind::NetLabel:
            error = c->ref + " has no value to sweep.";
            return runs;
        default: break;
    }
    if (options.values.empty() || options.values.size() > 100) {
        error = "A parameter sweep takes 1 to 100 values.";
        return runs;
    }
    for (const auto& value : options.values) {
        if (simulationStopRequested()) {
            error = kSimulationStopped;
            return {};
        }
        Schematic copy = schematic;
        copy.setValue(options.componentId, value);
        ParamSweepRun run;
        run.value = value;
        Simulator sim(copy);
        switch (options.analysis) {
            case SweepAnalysis::Dc: run.dc = sim.dcOperatingPoint(); break;
            case SweepAnalysis::Ac: run.ac = sim.ac(options.ac); break;
            case SweepAnalysis::Transient: run.transient = sim.transient(options.tStop, options.tStep); break;
        }
        runs.push_back(std::move(run));
    }
    return runs;
}

// ------------------------------------------------------------------ spectrum

namespace {
void fftInPlace(std::vector<std::complex<double>>& a) {
    const size_t n = a.size();
    for (size_t i = 1, j = 0; i < n; ++i) {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    for (size_t len = 2; len <= n; len <<= 1) {
        const double ang = -2 * kPi / static_cast<double>(len);
        for (size_t i = 0; i < n; i += len)
            for (size_t k = 0; k < len / 2; ++k) {
                // Twiddles computed directly (no recurrence) keep round-off at the level of a single cos/sin.
                std::complex<double> w(std::cos(ang * static_cast<double>(k)), std::sin(ang * static_cast<double>(k)));
                std::complex<double> u = a[i + k], v = a[i + k + len / 2] * w;
                a[i + k] = u + v;
                a[i + k + len / 2] = u - v;
            }
    }
}

// Linear interpolation of a sampled waveform (time ascending) at t.
double sampleAt(const std::vector<double>& time, const std::vector<double>& values, size_t n, double t, size_t& hint) {
    while (hint + 2 < n && time[hint + 1] < t) ++hint;
    double t0 = time[hint], t1 = time[hint + 1];
    double a = t1 > t0 ? std::clamp((t - t0) / (t1 - t0), 0.0, 1.0) : 0.0;
    return values[hint] + a * (values[hint + 1] - values[hint]);
}

std::vector<std::complex<double>> resampled(const std::vector<double>& time, const std::vector<double>& values,
                                            size_t n, double t0, double span, size_t points, bool hann) {
    std::vector<std::complex<double>> out(points);
    size_t hint = 0;
    while (hint + 2 < n && time[hint + 1] < t0) ++hint;
    for (size_t i = 0; i < points; ++i) {
        double t = t0 + span * static_cast<double>(i) / static_cast<double>(points);
        double w = hann ? 0.5 - 0.5 * std::cos(2 * kPi * static_cast<double>(i) / static_cast<double>(points)) : 1.0;
        out[i] = sampleAt(time, values, n, t, hint) * w;
    }
    return out;
}

size_t fftSize(size_t samples) {
    size_t n = 256;
    while (n < samples && n < 32768) n <<= 1;
    return n;
}
}  // namespace

SpectrumResult spectrum(const std::vector<double>& time, const std::vector<double>& values, double fundamentalHz,
                        int harmonics, double from) {
    SpectrumResult res;
    const size_t n = std::min(time.size(), values.size());
    if (n < 16) {
        res.error = "The waveform is too short for an FFT (at least 16 samples).";
        return res;
    }
    for (size_t i = 1; i < n; ++i)
        if (!(time[i] > time[i - 1])) {
            res.error = "The waveform's time points must increase.";
            return res;
        }
    const double tEnd = time[n - 1], begin = std::clamp(from, time[0], tEnd), record = tEnd - begin;
    if (!(record > 0)) {
        res.error = "The analysis window is empty: start it before the end of the waveform.";
        return res;
    }
    if (!(fundamentalHz > 0)) {
        // Strongest non-DC component of the Hann-windowed record, refined by parabolic interpolation over the bins.
        size_t points = fftSize(n);
        auto x = resampled(time, values, n, begin, record, points, true);
        fftInPlace(x);
        size_t best = 1;
        for (size_t k = 2; k < points / 2; ++k)
            if (std::abs(x[k]) > std::abs(x[best])) best = k;
        double delta = 0;
        if (best + 1 < points / 2) {
            double a = std::log(std::abs(x[best - 1]) + 1e-300), b = std::log(std::abs(x[best]) + 1e-300),
                   c = std::log(std::abs(x[best + 1]) + 1e-300);
            double d = a - 2 * b + c;
            if (d < 0) delta = std::clamp(0.5 * (a - c) / d, -0.5, 0.5);
        }
        fundamentalHz = (static_cast<double>(best) + delta) / record;
    }
    const double period = 1.0 / fundamentalHz;
    const int cycles = static_cast<int>(std::floor(record / period + 1e-9));
    if (cycles < 1) {
        res.error = "The waveform is shorter than one period of the fundamental (" +
                    formatEngineeringValue(fundamentalHz, "Hz") + "): run the transient analysis longer.";
        return res;
    }
    const double span = cycles * period, t0 = tEnd - span;
    // Samples inside the window decide the FFT size (power of two, at least 256, at most 32 768).
    size_t inside = 0;
    for (size_t i = 0; i < n; ++i)
        if (time[i] >= t0 - 1e-15) ++inside;
    const size_t points = fftSize(inside);
    auto x = resampled(time, values, n, t0, span, points, false);
    fftInPlace(x);
    auto amplitude = [&](size_t k) {
        return (k == 0 ? 1.0 : 2.0) * std::abs(x[k]) / static_cast<double>(points);
    };
    res.ok = true;
    res.fundamentalHz = fundamentalHz;
    res.cycles = cycles;
    res.windowStart = t0;
    res.windowStop = tEnd;
    res.dc = x[0].real() / static_cast<double>(points);
    const size_t bins = std::min<size_t>(points / 2, 4096);
    for (size_t k = 0; k <= bins; ++k) {
        res.frequency.push_back(static_cast<double>(k) / span);
        res.magnitudeDb.push_back(magnitudeDb(std::complex<double>(amplitude(k), 0.0)));
    }
    const double a1 = amplitude(static_cast<size_t>(cycles));
    double sum = 0;
    for (int h = 1; h <= std::max(1, harmonics); ++h) {
        size_t k = static_cast<size_t>(h) * static_cast<size_t>(cycles);
        if (k >= points / 2) break;
        Harmonic hm;
        hm.order = h;
        hm.frequency = h * fundamentalHz;
        hm.amplitude = amplitude(k);
        hm.dbc = a1 > 0 ? magnitudeDb(std::complex<double>(hm.amplitude / a1, 0.0)) : -400.0;
        if (h > 1) sum += hm.amplitude * hm.amplitude;
        res.harmonics.push_back(hm);
    }
    res.thd = a1 > 0 ? std::sqrt(sum) / a1 : 0.0;
    return res;
}

// ------------------------------------------------------------------ JSON

namespace {
// A number option: a JSON number or an engineering string ("10k", "5m"); `def` when missing or invalid.
double num(const Json& j, double def) {
    if (j.isNumber()) return j.asNumber();
    if (j.isString())
        if (auto v = parseEngineeringValue(j.asString())) return *v;
    return def;
}

int netByName(const Schematic& s, const Json& j) {
    const auto& nets = s.nets();
    if (j.isNumber()) {
        int i = j.asInt(-1);
        return i >= 0 && static_cast<size_t>(i) < nets.size() ? i : -1;
    }
    if (!j.isString()) return -1;
    std::string name = j.asString();
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

int componentByRef(const Schematic& s, const Json& j) {
    if (!j.isString() || j.asString().empty()) return -1;
    const Component* c = s.findByRef(j.asString());
    return c ? c->id : -1;
}

Json errorResult(const std::string& error) {
    Json j = Json::object();
    j["ok"] = false;
    j["error"] = error;
    return j;
}

// Nets worth reporting: not ground, connected to at least two pins (as the transient result lists them).
bool reportable(const Net& n) { return !n.isGround && n.pins.size() >= 2; }

Json numbers(const std::vector<double>& v) {
    Json a = Json::array();
    for (double d : v) a.push(d);
    return a;
}

Json metricsJson(const AcMetrics& m) {
    Json j = Json::object();
    j["lowFreqDb"] = m.lowFreqDb;
    j["peakDb"] = m.peakDb;
    j["peakHz"] = m.peakHz;
    j["f3dbHz"] = m.f3dbHz;  // NaN → null
    j["bwLowHz"] = m.bwLowHz;
    j["bwHighHz"] = m.bwHighHz;
    j["unityHz"] = m.unityHz;
    j["phaseMarginDeg"] = m.phaseMarginDeg;
    return j;
}

AcOptions acOptions(const Schematic& s, const Json& o, std::string& error) {
    AcOptions a;
    a.fStart = num(o.get("start"), 1.0);
    a.fStop = num(o.get("stop"), 1e6);
    a.pointsPerDecade = static_cast<int>(num(o.get("pointsPerDecade"), 50));
    if (o.has("source") && !(o.get("source").isString() && o.get("source").asString().empty())) {
        a.sourceId = componentByRef(s, o.get("source"));
        if (a.sourceId < 0) error = "Unknown AC input source '" + o.get("source").asString("") + "'.";
    }
    return a;
}

Json acNetsJson(const Schematic& s, const AcResult& r, int only) {
    Json nets = Json::array();
    for (const auto& n : s.nets()) {
        if (only >= 0 ? n.index != only : !reportable(n)) continue;
        const auto& h = r.netPhasors[static_cast<size_t>(n.index)];
        Json j = Json::object();
        j["index"] = n.index;
        j["name"] = n.name;
        j["dc"] = r.dcNetVoltages[static_cast<size_t>(n.index)];
        Json mag = Json::array();
        for (const auto& v : h) mag.push(magnitudeDb(v));
        j["magnitudeDb"] = mag;
        j["phaseDeg"] = numbers(unwrappedPhaseDeg(h));
        if (auto it = r.metrics.find(n.index); it != r.metrics.end()) j["metrics"] = metricsJson(it->second);
        nets.push(j);
    }
    return nets;
}

Json refsJson(const Schematic& s, const std::vector<int>& ids) {
    Json a = Json::array();
    for (int id : ids)
        if (const Component* c = s.find(id)) a.push(c->ref);
    return a;
}

// Every `stride`-th sample plus the last (keeps transient sweeps compact).
Json decimated(const std::vector<double>& v, size_t maxPoints) {
    size_t stride = v.size() > maxPoints ? (v.size() + maxPoints - 1) / maxPoints : 1;
    Json a = Json::array();
    for (size_t i = 0; i < v.size(); i += stride) a.push(v[i]);
    if (!v.empty() && (v.size() - 1) % stride != 0) a.push(v.back());
    return a;
}
}  // namespace

Json simulateAcJson(const Schematic& s, const Json& options) {
    std::string error;
    AcOptions o = acOptions(s, options, error);
    if (!error.empty()) return errorResult(error);
    Simulator sim(s);
    AcResult r = sim.ac(o);
    if (!r.ok) return errorResult(r.error);
    Json root = Json::object();
    root["ok"] = true;
    root["error"] = "";
    root["stimulus"] = refsJson(s, r.stimulus);
    root["frequency"] = numbers(r.frequency);
    root["nets"] = acNetsJson(s, r, -1);
    return root;
}

Json simulateDcSweepJson(const Schematic& s, const Json& options) {
    int id = componentByRef(s, options.get("source"));
    if (id < 0) return errorResult("Choose the source to sweep.");
    Simulator sim(s);
    DcSweepResult r = sim.dcSweep(id, num(options.get("start"), 0), num(options.get("stop"), 5),
                                  num(options.get("step"), 0.1));
    Json root = Json::object();
    root["ok"] = r.ok;
    root["error"] = r.error;
    root["source"] = options.get("source").asString("");
    const Component* c = s.find(id);
    root["unit"] = c && c->kind == ComponentKind::CurrentSource ? "A" : "V";
    root["values"] = numbers(r.values);
    Json nets = Json::array();
    for (const auto& n : s.nets()) {
        if (!reportable(n) || static_cast<size_t>(n.index) >= r.netVoltages.size()) continue;
        Json j = Json::object();
        j["index"] = n.index;
        j["name"] = n.name;
        j["values"] = numbers(r.netVoltages[static_cast<size_t>(n.index)]);
        nets.push(j);
    }
    root["nets"] = nets;
    Json currents = Json::array();
    for (const auto& [cid, values] : r.currents) {
        const Component* cc = s.find(cid);
        Json j = Json::object();
        j["component"] = cid;
        j["ref"] = cc ? cc->ref : std::string();
        j["values"] = numbers(values);
        currents.push(j);
    }
    root["currents"] = currents;
    return root;
}

Json simulateParamSweepJson(const Schematic& s, const Json& options) {
    ParamSweepOptions o;
    o.componentId = componentByRef(s, options.get("component"));
    if (o.componentId < 0) return errorResult("Choose the component to sweep.");
    const Json& values = options.get("values");
    if (values.isArray())
        for (size_t i = 0; i < values.size(); ++i) {
            const Json& v = values[i];
            if (v.isString()) o.values.push_back(v.asString());
            else if (v.isNumber()) o.values.push_back(formatEngineeringValue(v.asNumber(), "", 6));
        }
    std::string kind = options.get("analysis").asString("dc");
    if (kind == "ac") o.analysis = SweepAnalysis::Ac;
    else if (kind == "transient") o.analysis = SweepAnalysis::Transient;
    else if (kind == "dc") o.analysis = SweepAnalysis::Dc;
    else return errorResult("Unknown analysis '" + kind + "' (dc, ac or transient).");
    std::string error;
    o.ac = acOptions(s, options, error);
    if (!error.empty()) return errorResult(error);
    o.tStop = num(options.get("stop"), 1e-3);
    o.tStep = num(options.get("step"), 1e-6);
    int only = -1;
    if (options.has("net")) {
        only = netByName(s, options.get("net"));
        if (only < 0) return errorResult("Unknown net '" + options.get("net").asString("") + "'.");
        o.ac.measureNets = {only};
    }
    auto runs = parameterSweep(s, o, error);
    if (!error.empty()) return errorResult(error);
    Json root = Json::object();
    root["ok"] = true;
    root["error"] = "";
    root["component"] = options.get("component").asString("");
    root["analysis"] = kind;
    Json out = Json::array();
    for (const auto& run : runs) {
        Json j = Json::object();
        j["value"] = run.value;
        j["ok"] = run.ok();
        j["error"] = run.ok() ? std::string() : run.error();
        Json nets = Json::array();
        if (run.ok()) {
            if (o.analysis == SweepAnalysis::Ac) {
                j["x"] = numbers(run.ac.frequency);
                nets = acNetsJson(s, run.ac, only);
            } else {
                const TransientResult& tr = run.transient;
                if (o.analysis == SweepAnalysis::Transient) j["x"] = decimated(tr.time, 1000);
                for (const auto& n : s.nets()) {
                    if (only >= 0 ? n.index != only : !reportable(n)) continue;
                    Json nj = Json::object();
                    nj["index"] = n.index;
                    nj["name"] = n.name;
                    if (o.analysis == SweepAnalysis::Dc) nj["voltage"] = run.dc.netVoltages[static_cast<size_t>(n.index)];
                    else nj["values"] = decimated(tr.netVoltages[static_cast<size_t>(n.index)], 1000);
                    nets.push(nj);
                }
            }
        }
        j["nets"] = nets;
        out.push(j);
    }
    root["runs"] = out;
    return root;
}

Json simulateToleranceJson(const Schematic& s, const Json& options) {
    Measurement m;
    m.net = netByName(s, options.get("net"));
    if (m.net < 0) return errorResult("Choose the net to measure.");
    std::string what = options.get("measure").asString("dc");
    std::string unit = "V";
    if (what == "gain") {
        m.kind = Measurement::Kind::AcGainDb;
        unit = "dB";
    } else if (what == "f3db" || what == "peak") {
        m.kind = what == "f3db" ? Measurement::Kind::AcF3db : Measurement::Kind::AcPeakHz;
        unit = "Hz";
    } else if (what != "dc") {
        return errorResult("Unknown measurement '" + what + "' (dc, gain, f3db or peak).");
    }
    std::string error;
    m.ac = acOptions(s, options, error);
    if (!error.empty()) return errorResult(error);
    ToleranceOptions o;
    o.runs = static_cast<int>(num(options.get("runs"), 100));
    o.seed = static_cast<uint64_t>(std::max(0.0, num(options.get("seed"), 1)));
    o.gaussian = options.get("distribution").asString("uniform") == "gaussian";
    o.worstCase = options.get("worstCase").asBool(true);
    const Json& tol = options.get("tolerances");
    o.defaults.resistor = num(tol.get("resistor"), o.defaults.resistor);
    o.defaults.capacitor = num(tol.get("capacitor"), o.defaults.capacitor);
    o.defaults.inductor = num(tol.get("inductor"), o.defaults.inductor);
    ToleranceResult r = toleranceAnalysis(s, m, o);
    if (!r.ok) return errorResult(r.error);

    Json root = Json::object();
    root["ok"] = true;
    root["error"] = "";
    root["net"] = s.nets()[static_cast<size_t>(m.net)].name;
    root["measure"] = what;
    root["unit"] = unit;
    root["seed"] = static_cast<double>(o.seed);
    root["distribution"] = o.gaussian ? "gaussian" : "uniform";
    root["nominal"] = r.nominal;
    root["runs"] = static_cast<int>(r.samples.size());
    root["failedRuns"] = r.failedRuns;
    root["min"] = r.min;
    root["max"] = r.max;
    root["mean"] = r.mean;
    root["sigma"] = r.sigma;
    root["samples"] = numbers(r.samples);
    // 20-bin histogram over [min, max].
    Json hist = Json::object();
    Json edges = Json::array(), counts = Json::array();
    if (!r.samples.empty()) {
        const int bins = 20;
        double lo = r.min, hi = r.max > r.min ? r.max : r.min + std::max(1e-12, std::fabs(r.min) * 1e-9);
        std::vector<int> c(bins, 0);
        for (double v : r.samples) c[static_cast<size_t>(std::clamp(static_cast<int>((v - lo) / (hi - lo) * bins), 0, bins - 1))]++;
        for (int i = 0; i <= bins; ++i) edges.push(lo + (hi - lo) * i / bins);
        for (int v : c) counts.push(v);
    }
    hist["edges"] = edges;
    hist["counts"] = counts;
    root["histogram"] = hist;
    if (r.hasWorstCase) {
        Json wc = Json::object();
        wc["min"] = r.worstMin;
        wc["max"] = r.worstMax;
        auto settings = [&](const std::map<int, double>& scale) {
            Json a = Json::array();
            for (const auto& [id, factor] : scale) {
                if (std::fabs(factor - 1.0) < 1e-15) continue;
                Json p = Json::object();
                const Component* c = s.find(id);
                p["ref"] = c ? c->ref : std::string();
                p["factor"] = factor;
                a.push(p);
            }
            return a;
        };
        wc["minParts"] = settings(r.worstMinScale);
        wc["maxParts"] = settings(r.worstMaxScale);
        root["worstCase"] = wc;
    }
    Json parts = Json::array();
    for (const auto& p : r.parts) {
        const Component* c = s.find(p.componentId);
        Json j = Json::object();
        j["component"] = p.componentId;
        j["ref"] = c ? c->ref : std::string();
        j["value"] = c ? c->value : std::string();
        j["tolerance"] = p.tolerance;
        j["fromValue"] = p.fromValue;
        if (r.hasWorstCase) j["sensitivity"] = p.sensitivity;
        parts.push(j);
    }
    root["parts"] = parts;
    return root;
}

Json simulateFftJson(const Schematic& s, const Json& options) {
    int net = netByName(s, options.get("net"));
    if (net < 0) return errorResult("Choose the net to analyse.");
    double fundamental = num(options.get("fundamental"), 0);
    if (!(fundamental > 0))  // default: the first sine source's frequency
        for (const auto& c : s.components())
            if (isVoltageSourceKind(c.kind) || c.kind == ComponentKind::CurrentSource)
                if (auto spec = SourceSpec::parse(c.value); spec && spec->kind == SourceSpec::Kind::Sine) {
                    fundamental = spec->frequency;
                    break;
                }
    const double stop = num(options.get("stop"), 10e-3);
    Simulator sim(s);
    TransientResult tr = sim.transient(stop, num(options.get("step"), 1e-6));
    if (!tr.ok) return errorResult(tr.error);
    // Default window: the second half of the run, after start-up transients have (usually) settled.
    SpectrumResult r = spectrum(tr.time, tr.netVoltages[static_cast<size_t>(net)], fundamental,
                                static_cast<int>(num(options.get("harmonics"), 10)), num(options.get("from"), stop / 2));
    if (!r.ok) return errorResult(r.error);
    Json root = Json::object();
    root["ok"] = true;
    root["error"] = "";
    root["net"] = s.nets()[static_cast<size_t>(net)].name;
    root["fundamentalHz"] = r.fundamentalHz;
    root["thdPercent"] = r.thd * 100.0;
    root["dc"] = r.dc;
    root["cycles"] = r.cycles;
    root["windowStart"] = r.windowStart;
    root["windowStop"] = r.windowStop;
    root["frequency"] = numbers(r.frequency);
    root["magnitudeDb"] = numbers(r.magnitudeDb);
    Json hs = Json::array();
    for (const auto& h : r.harmonics) {
        Json j = Json::object();
        j["order"] = h.order;
        j["frequency"] = h.frequency;
        j["amplitude"] = h.amplitude;
        j["dbc"] = h.dbc;
        hs.push(j);
    }
    root["harmonics"] = hs;
    return root;
}

}  // namespace sieda
