// SiEDA Core — schematic directives: net classes, differential-pair markers and parameter sets on nets.
//
// The schematic is the source of the board's net rules (as in Altium): a directive sits on a component pin and gives
// that pin's net a net class (track width, clearance), marks it as a differential pair member (paired with the net of
// the opposite suffix, X_P / X_N), or sets its own width and clearance. Project::applySchematicRules carries the
// result into the board settings (net widths, net clearances) that the autorouter, the interactive router and DRC use;
// differential pairs join the name-based ones for routing, length tuning and the signal-integrity checks.
#include <algorithm>
#include <cctype>
#include <cmath>
#include <map>
#include <set>

#include "sieda/Schematic.hpp"

namespace sieda {

namespace {
bool validClassName(const std::string& s) {
    if (s.empty() || s.size() > 32) return false;
    return std::all_of(s.begin(), s.end(), [](char ch) {
        return std::isalnum(static_cast<unsigned char>(ch)) != 0 || ch == '_' || ch == '-';
    });
}

/// 0 (not set) or a sensible copper dimension in mm.
bool validSize(double v) { return v == 0 || (std::isfinite(v) && v >= 0.05 && v <= 10); }

std::string upperName(std::string s) {
    for (auto& ch : s) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    return s;
}

/// Differential suffixes, positive then negative.
const std::pair<const char*, const char*> kPairSuffixes[] = {
    {"_P", "_N"}, {"+", "-"}, {"_DP", "_DN"}, {".DP", ".DN"}, {"CANH", "CANL"}, {"_H", "_L"}, {"P", "N"}};
}  // namespace

const NetClassDef* Schematic::findNetClassDef(const std::string& name) const {
    for (const auto& d : netClassDefs_)
        if (d.name == name) return &d;
    return nullptr;
}

bool Schematic::setNetClassDef(const NetClassDef& def) {
    if (!validClassName(def.name) || !validSize(def.trackWidth) || !validSize(def.clearance)) return false;
    for (auto& d : netClassDefs_)
        if (d.name == def.name) {
            d = def;
            return true;
        }
    if (netClassDefs_.size() >= 256) return false;
    netClassDefs_.push_back(def);
    return true;
}

bool Schematic::removeNetClassDef(const std::string& name) {
    const auto it = std::find_if(netClassDefs_.begin(), netClassDefs_.end(), [&](const NetClassDef& d) { return d.name == name; });
    if (it == netClassDefs_.end()) return false;
    netClassDefs_.erase(it);
    return true;
}

const NetDirective* Schematic::findDirective(int id) const {
    for (const auto& d : directives_)
        if (d.id == id) return &d;
    return nullptr;
}

int Schematic::addDirective(const NetDirective& raw) {
    NetDirective d = raw;
    d.component = masterOf(d.component);
    const Component* c = find(d.component);
    if (!c || d.pin < 0 || d.pin >= static_cast<int>(c->def().pins.size()) || c->kind == ComponentKind::Junction) return -1;
    if (!validSize(d.trackWidth) || !validSize(d.clearance)) return -1;
    if (!d.netClass.empty() && !validClassName(d.netClass)) return -1;
    if (directives_.size() >= 4096) return -1;
    d.id = nextDirectiveId_++;
    directives_.push_back(d);
    return d.id;
}

bool Schematic::updateDirective(int id, const NetDirective& raw) {
    for (auto& d : directives_) {
        if (d.id != id) continue;
        NetDirective next = raw;
        next.id = id;
        next.component = masterOf(next.component < 0 ? d.component : next.component);
        const Component* c = find(next.component);
        if (!c || next.pin < 0 || next.pin >= static_cast<int>(c->def().pins.size())) return false;
        if (!validSize(next.trackWidth) || !validSize(next.clearance)) return false;
        if (!next.netClass.empty() && !validClassName(next.netClass)) return false;
        d = next;
        return true;
    }
    return false;
}

bool Schematic::removeDirective(int id) {
    const auto it = std::find_if(directives_.begin(), directives_.end(), [&](const NetDirective& d) { return d.id == id; });
    if (it == directives_.end()) return false;
    directives_.erase(it);
    return true;
}

void Schematic::restoreDirective(const NetDirective& raw) {
    NetDirective d = raw;
    const Component* c = find(d.component);
    if (d.id <= 0 || findDirective(d.id) || !c || d.pin < 0 || d.pin >= static_cast<int>(c->def().pins.size())) return;
    if (!validSize(d.trackWidth) || !validSize(d.clearance) || (!d.netClass.empty() && !validClassName(d.netClass))) return;
    directives_.push_back(d);
    nextDirectiveId_ = std::max(nextDirectiveId_, nextIdAfter(d.id));
}

bool Schematic::repairDirectives() {
    const size_t before = directives_.size();
    directives_.erase(std::remove_if(directives_.begin(), directives_.end(),
                                     [&](const NetDirective& d) {
                                         const Component* c = find(d.component);
                                         return !c || d.pin >= static_cast<int>(c->def().pins.size());
                                     }),
                      directives_.end());
    for (auto& d : directives_) d.component = masterOf(d.component);  // a copy that joined its block
    return directives_.size() != before;
}

namespace {
/// The nets a directive reaches: its pin's net and the same pin's net on every channel copy.
std::vector<int> directiveNets(const Schematic& s, const NetDirective& d) {
    std::vector<int> out;
    const int first = s.netOf({d.component, d.pin});
    if (first >= 0) out.push_back(first);
    for (const auto& c : s.components())
        if (c.instanceOf == d.component) {
            const int n = s.netOf({c.id, d.pin});
            if (n >= 0 && std::find(out.begin(), out.end(), n) == out.end()) out.push_back(n);
        }
    return out;
}
}  // namespace

std::vector<std::pair<int, int>> Schematic::directiveDiffPairs() const {
    std::vector<std::pair<int, int>> out;
    if (directives_.empty()) return out;
    std::set<int> marked;
    for (const auto& d : directives_)
        if (d.diffPair)
            for (int n : directiveNets(*this, d)) marked.insert(n);
    if (marked.empty()) return out;
    std::map<std::string, int> byName;
    for (const auto& n : nets())
        if (!n.isGround) byName.emplace(upperName(n.name), n.index);
    std::set<std::pair<int, int>> seen;
    for (int net : marked) {
        const std::string name = upperName(nets()[static_cast<size_t>(net)].name);
        for (const auto& [pos, neg] : kPairSuffixes) {
            const std::string p = pos, n = neg;
            int partner = -1;
            bool positive = false;
            if (name.size() > p.size() && name.compare(name.size() - p.size(), p.size(), p) == 0) {
                auto it = byName.find(name.substr(0, name.size() - p.size()) + n);
                if (it != byName.end()) {
                    partner = it->second;
                    positive = true;
                }
            } else if (name.size() > n.size() && name.compare(name.size() - n.size(), n.size(), n) == 0) {
                auto it = byName.find(name.substr(0, name.size() - n.size()) + p);
                if (it != byName.end()) partner = it->second;
            }
            if (partner < 0 || partner == net) continue;
            const std::pair<int, int> pr = positive ? std::make_pair(net, partner) : std::make_pair(partner, net);
            if (seen.insert(pr).second) out.push_back(pr);
            break;
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::vector<NetRule> Schematic::netRules() const {
    std::vector<NetRule> out;
    if (directives_.empty()) return out;
    std::map<int, NetRule> rules;
    for (const auto& d : directives_) {
        for (int net : directiveNets(*this, d)) {
            NetRule& r = rules[net];
            r.net = net;
            r.netName = nets()[static_cast<size_t>(net)].name;
            if (!d.netClass.empty()) {
                r.netClass = d.netClass;
                if (const NetClassDef* c = findNetClassDef(d.netClass)) {
                    if (c->trackWidth > 0) r.trackWidth = std::max(r.trackWidth, c->trackWidth);
                    if (c->clearance > 0) r.clearance = std::max(r.clearance, c->clearance);
                }
            }
            r.diffPair = r.diffPair || d.diffPair;
        }
    }
    // A parameter set on the net wins over its class.
    for (const auto& d : directives_)
        for (int net : directiveNets(*this, d)) {
            NetRule& r = rules[net];
            if (d.trackWidth > 0) r.trackWidth = d.trackWidth;
            if (d.clearance > 0) r.clearance = d.clearance;
        }
    for (const auto& [p, n] : directiveDiffPairs()) {
        rules[p].partner = n;
        rules[n].partner = p;
        rules[p].net = p;
        rules[n].net = n;
        rules[p].diffPair = rules[n].diffPair = true;
        rules[p].netName = nets()[static_cast<size_t>(p)].name;
        rules[n].netName = nets()[static_cast<size_t>(n)].name;
    }
    for (auto& [net, r] : rules) out.push_back(r);
    return out;
}

void Schematic::directiveERC(std::vector<RuleViolation>& out) const {
    auto add = [&](Severity s, const char* code, const std::string& msg, const NetDirective& d) {
        RuleViolation v;
        v.severity = s;
        v.code = code;
        v.message = msg;
        if (const Component* c = find(d.component)) {
            v.components = {c->id};
            v.location = c->position;
            v.hasLocation = true;
            v.sheet = c->sheet;
        }
        out.push_back(std::move(v));
    };
    std::map<int, std::set<std::string>> classesOf;
    std::set<int> pairedNets;
    for (const auto& [p, n] : directiveDiffPairs()) {
        pairedNets.insert(p);
        pairedNets.insert(n);
    }
    for (const auto& d : directives_) {
        const Component* c = find(d.component);
        const std::string where = c ? c->ref : std::string("?");
        if (!d.netClass.empty() && !findNetClassDef(d.netClass))
            add(Severity::Error, "ERC_DIRECTIVE_UNKNOWN_CLASS",
                "Directive on " + where + " names net class " + d.netClass + ", which is not defined (Net Classes).", d);
        const auto nets = directiveNets(*this, d);
        if (nets.empty() || !isPinConnected({d.component, d.pin})) {
            add(Severity::Warning, "ERC_DIRECTIVE_NO_NET",
                "Directive on " + where + " is on an unconnected pin: it applies to no net of the circuit.", d);
            continue;
        }
        for (int n : nets) {
            if (!d.netClass.empty()) classesOf[n].insert(d.netClass);
            if (d.diffPair && !pairedNets.count(n))
                add(Severity::Warning, "ERC_DIFF_PAIR_UNPAIRED",
                    "Net " + this->nets()[static_cast<size_t>(n)].name +
                        " is marked as a differential pair member but has no partner (name the nets X_P / X_N).", d);
        }
    }
    for (const auto& [net, classes] : classesOf)
        if (classes.size() > 1) {
            std::string list;
            for (const auto& c : classes) list += (list.empty() ? "" : ", ") + c;
            RuleViolation v;
            v.severity = Severity::Warning;
            v.code = "ERC_DIRECTIVE_CONFLICT";
            v.message = "Net " + nets()[static_cast<size_t>(net)].name + " is given several net classes (" + list +
                        "): the widest width and clearance apply.";
            out.push_back(std::move(v));
        }
}

}  // namespace sieda
