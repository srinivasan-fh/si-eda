#include "sieda/Touchstone.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <stdexcept>

#include "sieda/Eye.hpp"

namespace sieda {

namespace {

constexpr int kMaxPorts = 64;
constexpr size_t kMaxText = 64u << 20;     // 64 MB
constexpr size_t kMaxValues = 40u << 20;   // numbers in the network data

std::string upper(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}

std::vector<std::string> split(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (std::isspace(static_cast<unsigned char>(c)) || c == ',') {
            if (!cur.empty()) out.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

bool number(const std::string& t, double& v) {
    if (t.empty() || t.size() > 64) return false;
    char* end = nullptr;
    v = std::strtod(t.c_str(), &end);
    return end && *end == '\0' && std::isfinite(v);
}

[[noreturn]] void fail(const std::string& why) { throw std::runtime_error("Touchstone: " + why); }

}  // namespace

int touchstonePortsFromName(const std::string& fileName) {
    const std::string n = upper(fileName);
    const size_t dot = n.rfind('.');
    if (dot == std::string::npos || n.size() < dot + 4 || n[dot + 1] != 'S' || n.back() != 'P') return 0;
    const std::string digits = n.substr(dot + 2, n.size() - dot - 3);
    if (digits.empty() || digits.size() > 3 || !std::all_of(digits.begin(), digits.end(), ::isdigit)) return 0;
    const int p = std::atoi(digits.c_str());
    return p >= 1 && p <= kMaxPorts ? p : 0;
}

TouchstoneData parseTouchstone(const std::string& text, int portsHint) {
    if (text.size() > kMaxText) fail("file too large");
    TouchstoneData t;
    t.version = "1.1";
    double unit = 1e9, refR = 50;
    std::string param = "S", format = "MA";
    bool option = false, inData = false, v2 = false, ended = false;
    int ports = portsHint > 0 && portsHint <= kMaxPorts ? portsHint : 0;
    std::string order = "21_12", matrix = "FULL";
    long declaredFreqs = -1;
    bool expectReference = false;
    std::vector<double> refs;
    std::vector<double> values;
    std::vector<double> freqs;
    std::vector<std::vector<double>> groups;
    size_t perFreq = 0;
    int firstLineCount = -1;
    bool noise = false;
    std::istringstream in(text);
    std::string raw;
    size_t lineNo = 0;
    auto groupSize = [&]() -> size_t {
        const size_t n = static_cast<size_t>(ports);
        return 1 + 2 * (matrix == "FULL" ? n * n : n * (n + 1) / 2);
    };
    while (std::getline(in, raw)) {
        ++lineNo;
        if (ended || noise) break;
        std::string line = raw;
        const size_t bang = line.find('!');
        if (bang != std::string::npos) {
            const std::string c = trim(line.substr(bang + 1));
            if (!c.empty() && t.comments.size() < 50) t.comments.push_back(c.substr(0, 200));
            line = line.substr(0, bang);
        }
        line = trim(line);
        if (line.empty()) continue;
        if (line[0] == '#') {
            if (option) continue;  // only the first option line counts
            option = true;
            for (const auto& tok : split(line.substr(1))) {
                const std::string u = upper(tok);
                if (u == "HZ") unit = 1;
                else if (u == "KHZ") unit = 1e3;
                else if (u == "MHZ") unit = 1e6;
                else if (u == "GHZ") unit = 1e9;
                else if (u == "S" || u == "Y" || u == "Z") param = u;
                else if (u == "G" || u == "H") fail("hybrid (" + u + ") parameters are not supported");
                else if (u == "MA" || u == "DB" || u == "RI") format = u;
                else if (u == "R") expectReference = true;
                else if (expectReference) {
                    double r;
                    if (!number(tok, r) || !(r > 0) || r > 1e6) fail("bad reference impedance in the option line");
                    refR = r;
                    expectReference = false;
                } else {
                    fail("unknown option '" + tok.substr(0, 20) + "'");
                }
            }
            if (expectReference) fail("missing reference impedance after R");
            continue;
        }
        if (line[0] == '[') {
            const size_t close = line.find(']');
            if (close == std::string::npos) fail("unterminated keyword on line " + std::to_string(lineNo));
            const std::string key = upper(trim(line.substr(1, close - 1)));
            const std::string rest = trim(line.substr(close + 1));
            const auto toks = split(rest);
            double v = 0;
            if (key == "VERSION") {
                v2 = true;
                t.version = rest.empty() ? "2.0" : rest.substr(0, 10);
            } else if (key == "NUMBER OF PORTS") {
                if (toks.empty() || !number(toks[0], v) || v < 1 || v > kMaxPorts || v != std::floor(v)) fail("bad [Number of Ports]");
                ports = static_cast<int>(v);
            } else if (key == "TWO-PORT DATA ORDER") {
                const std::string o = toks.empty() ? "" : upper(toks[0]);
                if (o != "12_21" && o != "21_12") fail("bad [Two-Port Data Order]");
                order = o;
            } else if (key == "NUMBER OF FREQUENCIES") {
                if (toks.empty() || !number(toks[0], v) || v < 1 || v > 1e7) fail("bad [Number of Frequencies]");
                declaredFreqs = static_cast<long>(v);
            } else if (key == "REFERENCE") {
                for (const auto& tok : toks) {
                    if (!number(tok, v) || !(v > 0)) fail("bad [Reference]");
                    refs.push_back(v);
                }
                expectReference = refs.empty();
            } else if (key == "MATRIX FORMAT") {
                const std::string m = toks.empty() ? "" : upper(toks[0]);
                if (m != "FULL" && m != "LOWER" && m != "UPPER") fail("bad [Matrix Format]");
                matrix = m;
            } else if (key == "MIXED-MODE ORDER") {
                fail("mixed-mode Touchstone data is not supported; export single-ended data");
            } else if (key == "NETWORK DATA") {
                inData = true;
            } else if (key == "NOISE DATA") {
                noise = true;
            } else if (key == "END") {
                ended = true;
            }
            continue;
        }
        const auto toks = split(line);
        if (expectReference && v2 && !inData) {
            for (const auto& tok : toks) {
                double v;
                if (!number(tok, v) || !(v > 0)) fail("bad [Reference]");
                refs.push_back(v);
            }
            expectReference = false;
            continue;
        }
        if (!option) fail("data before the option line (# …)");
        if (v2 && !inData) fail("data before [Network Data]");
        if (ports == 0) {
            // Version 1 without a port count: the first data line tells 1, 2 and 3 ports apart.
            const int c = static_cast<int>(toks.size());
            ports = c == 3 ? 1 : c == 9 ? 2 : c == 7 ? 3 : 0;
            if (ports == 0) fail("cannot tell the number of ports: name the file .sNp");
        }
        if (perFreq == 0) perFreq = groupSize();
        if (firstLineCount < 0) firstLineCount = static_cast<int>(toks.size());
        // Version 1 two-port files may end with noise parameters: their first frequency is not above the last one.
        if (values.empty() && !groups.empty()) {
            double f0;
            if (!toks.empty() && number(toks[0], f0) && f0 * unit <= freqs.back()) {
                noise = true;
                break;
            }
        }
        for (const auto& tok : toks) {
            double v;
            if (!number(tok, v)) fail("not a number on line " + std::to_string(lineNo));
            values.push_back(v);
            if (values.size() == perFreq) {
                const double f = values[0] * unit;
                if (!(f >= 0) || (!freqs.empty() && !(f > freqs.back()))) fail("frequencies must increase (line " + std::to_string(lineNo) + ")");
                freqs.push_back(f);
                groups.push_back(values);
                values.clear();
                if (groups.size() * perFreq > kMaxValues) fail("too much data");
            }
        }
    }
    if (!option && !v2) fail("no option line (# …): not a Touchstone file");
    if (ports == 0) fail("no network data");
    if (!values.empty()) fail("incomplete data for the last frequency");
    if (groups.empty()) fail("no network data");
    if (declaredFreqs > 0 && static_cast<long>(groups.size()) != declaredFreqs) fail("[Number of Frequencies] does not match the data");
    if (!refs.empty()) {
        for (double r : refs)
            if (std::fabs(r - refs[0]) > 1e-9 * refs[0]) fail("different reference impedances per port are not supported");
        refR = refs[0];
    }
    t.format = format;
    t.parameter = param;
    t.sp.ports = ports;
    t.sp.z0 = refR;
    t.sp.freq = freqs;
    const int n = ports;
    for (const auto& g : groups) {
        CMat m(n);
        size_t k = 1;
        auto next = [&]() {
            const double a = g[k], b = g[k + 1];
            k += 2;
            if (format == "RI") return cplx(a, b);
            const double mag = format == "DB" ? std::pow(10.0, a / 20) : a;
            return std::polar(mag, b * kPi / 180);
        };
        if (n == 2 && matrix == "FULL") {
            const cplx a = next(), b = next(), c = next(), d = next();
            m(0, 0) = a;
            m(1, 1) = d;
            if (order == "21_12") {
                m(1, 0) = b;
                m(0, 1) = c;
            } else {
                m(0, 1) = b;
                m(1, 0) = c;
            }
        } else {
            for (int i = 0; i < n; ++i)
                for (int j = 0; j < n; ++j) {
                    if (matrix == "LOWER" && j > i) continue;
                    if (matrix == "UPPER" && j < i) continue;
                    m(i, j) = next();
                    if (matrix != "FULL") m(j, i) = m(i, j);
                }
        }
        // Y / Z to S (version 1 normalises them to R; version 2 does not).
        if (param != "S") {
            const double scale = v2 ? 1.0 : (param == "Z" ? refR : 1 / refR);
            for (auto& v : m.a) v *= scale;
            CMat x = m;  // Z or Y in ohms / siemens
            CMat num(n), den(n);
            for (int i = 0; i < n; ++i)
                for (int j = 0; j < n; ++j) {
                    const cplx v = param == "Z" ? x(i, j) : x(i, j) * refR;
                    const cplx id = i == j ? 1.0 : 0.0;
                    if (param == "Z") {
                        num(i, j) = v - id * refR;
                        den(i, j) = v + id * refR;
                    } else {
                        num(i, j) = id - v;
                        den(i, j) = id + v;
                    }
                }
            if (!invertInPlace(den)) fail("singular " + param + "-parameters");
            m = param == "Z" ? num * den : den * num;
        }
        for (const auto& v : m.a)
            if (!std::isfinite(v.real()) || !std::isfinite(v.imag())) fail("non-finite parameters");
        t.sp.s.push_back(m);
    }
    return t;
}

std::string writeTouchstone(const SParams& sp, const std::string& comment) {
    std::string out;
    char buf[64];
    if (!comment.empty()) out += "! " + comment + "\n";
    out += "! Touchstone 1.1, ";
    out += std::to_string(sp.ports) + " ports, written by SiEDA\n";
    std::snprintf(buf, sizeof buf, "# Hz S RI R %.6g\n", sp.z0);
    out += buf;
    auto pair = [&](cplx v) {
        std::snprintf(buf, sizeof buf, " %.10g %.10g", v.real(), v.imag());
        out += buf;
    };
    const int n = sp.ports;
    for (size_t k = 0; k < sp.freq.size() && k < sp.s.size(); ++k) {
        std::snprintf(buf, sizeof buf, "%.10g", sp.freq[k]);
        out += buf;
        const CMat& m = sp.s[k];
        if (n == 2) {
            pair(m(0, 0));
            pair(m(1, 0));
            pair(m(0, 1));
            pair(m(1, 1));
            out += "\n";
            continue;
        }
        for (int i = 0; i < n; ++i) {
            for (int j = 0; j < n; ++j) {
                if (j > 0 && j % 4 == 0) out += "\n";
                pair(m(i, j));
            }
            out += "\n";
        }
    }
    return out;
}

SParams reorderFourPort(const SParams& sp, const std::string& order) {
    if (sp.ports != 4 || order != "12") return sp;
    const int perm[4] = {0, 2, 1, 3};
    SParams out = sp;
    for (size_t k = 0; k < sp.s.size(); ++k)
        for (int i = 0; i < 4; ++i)
            for (int j = 0; j < 4; ++j) out.s[k](i, j) = sp.s[k](perm[i], perm[j]);
    return out;
}

namespace {

double dbOf(cplx v) { return 20 * std::log10(std::max(1e-12, std::abs(v))); }

Json previewCurves(const SParams& sp, std::vector<double>* freqOut) {
    Json curves = Json::array();
    const size_t F = sp.freq.size();
    const size_t step = std::max<size_t>(1, F / 1000);
    std::vector<std::string> names;
    if (sp.ports == 4) names = {"SDD21", "SDD11", "SCD21"};
    else if (sp.ports >= 2) names = {"S21", "S11"};
    else names = {"S11"};
    std::vector<Json> data(names.size(), Json::array());
    for (size_t k = 0; k < F; k += step) {
        if (freqOut) freqOut->push_back(sp.freq[k]);
        const CMat& s = sp.s[k];
        if (sp.ports == 4) {
            const CMat m = mixedMode(s);
            data[0].push(dbOf(m(1, 0)));
            data[1].push(dbOf(m(0, 0)));
            data[2].push(dbOf(m(3, 0)));
        } else if (sp.ports >= 2) {
            data[0].push(dbOf(s(1, 0)));
            data[1].push(dbOf(s(0, 0)));
        } else {
            data[0].push(dbOf(s(0, 0)));
        }
    }
    for (size_t i = 0; i < names.size(); ++i) {
        Json c = Json::object();
        c["name"] = names[i];
        c["db"] = data[i];
        curves.push(c);
    }
    return curves;
}

Json numbers(const std::vector<double>& v) {
    Json a = Json::array();
    for (double x : v) a.push(std::isfinite(x) ? x : 0.0);
    return a;
}

}  // namespace

Json touchstoneJson(const TouchstoneData& t, const std::string& order) {
    Json j = Json::object();
    const SParams sp = reorderFourPort(t.sp, order);
    j["ports"] = sp.ports;
    j["z0"] = sp.z0;
    j["points"] = static_cast<int>(sp.freq.size());
    j["fMin"] = sp.freq.empty() ? 0.0 : sp.freq.front();
    j["fMax"] = sp.freq.empty() ? 0.0 : sp.freq.back();
    j["format"] = t.format;
    j["parameter"] = t.parameter;
    j["version"] = t.version;
    std::vector<double> f;
    j["curves"] = previewCurves(sp, &f);
    j["freq"] = numbers(f);
    Json c = Json::array();
    for (const auto& x : t.comments) c.push(x);
    j["comments"] = c;
    return j;
}

Json touchstoneChannelJson(const TouchstoneData& t, const std::string& order, const ChannelDrive& drive, const EyeOptions* eye) {
    Json j = touchstoneJson(t, order);
    const SParams sp = reorderFourPort(t.sp, order);
    if (sp.ports != 2 && sp.ports != 4) {
        j["error"] = "A channel needs a 2-port (single-ended) or 4-port (differential) file";
        return j;
    }
    const double z0 = sp.z0;
    auto transfer = [&](const std::vector<double>& freq) {
        const SParams s = interpolateSParams(sp, freq);
        std::vector<cplx> h(freq.size(), cplx(0, 0));
        for (size_t k = 0; k < freq.size(); ++k) {
            const PortTermination rx = PortTermination::load(1.0 / drive.termOhms, z0);
            if (sp.ports == 2) {
                h[k] = terminatedVoltages(s.s[k], z0, {PortTermination::source(1.0, drive.sourceOhms, z0), rx})[1];
            } else {
                const auto v = terminatedVoltages(s.s[k], z0, {PortTermination::source(1.0, drive.sourceOhms, z0),
                                                               PortTermination::source(-1.0, drive.sourceOhms, z0), rx, rx});
                h[k] = v[2] - v[3];
            }
        }
        return h;
    };
    // Delay: the group delay of the thru path at mid band.
    double delay = 0;
    if (sp.freq.size() > 2) {
        const size_t a = sp.freq.size() / 4, b = sp.freq.size() / 2;
        const int r = sp.ports == 2 ? 1 : 2;
        double pa = std::arg(sp.s[a](r, 0)), acc = 0, prev = pa;
        for (size_t k = a + 1; k <= b; ++k) {
            double p = std::arg(sp.s[k](r, 0));
            while (p - prev > kPi) p -= 2 * kPi;
            while (p - prev < -kPi) p += 2 * kPi;
            acc += p - prev;
            prev = p;
        }
        const double df = sp.freq[b] - sp.freq[a];
        if (df > 0) delay = std::clamp(-acc / (2 * kPi * df), 0.0, 1e-6);
    }
    const double rise = drive.riseTime > 0 ? drive.riseTime : eye ? 0.25 / eye->bitRate : 50e-12;
    const double tStop = std::max({20 * delay + 10 * rise, 2e-9, 6 * rise});
    const double dt = std::max(tStop / 4000, rise / 20);
    const auto st = stepResponse(transfer, rise, dt, tStop);
    const double vLow = sp.ports == 4 ? -drive.swing * std::real(transfer({0.0})[0]) : 0.0;
    std::vector<double> time, v;
    const size_t step = std::max<size_t>(1, st.size() / 500);
    for (size_t k = 0; k < st.size(); k += step) {
        time.push_back(static_cast<double>(k) * dt);
        v.push_back(vLow + drive.swing * st[k]);
    }
    Json s = Json::object();
    s["time"] = numbers(time);
    s["lossy"] = numbers(v);
    j["step"] = s;
    j["delay"] = delay;
    if (eye) {
        EyeOptions eo = *eye;
        if (!(eo.riseTime > 0)) eo.riseTime = rise;
        const double vMid = sp.ports == 4 ? 0.0 : std::real(transfer({0.0})[0]) * drive.swing / 2;
        j["eye"] = eyeJson(simulateEye(transfer, drive.swing / 2, vMid, delay, eo));
    }
    return j;
}

}  // namespace sieda
