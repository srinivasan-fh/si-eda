#include "sieda/Verification.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <set>
#include <sstream>

#include "sieda/Export.hpp"
#include "sieda/Industry.hpp"
#include "sieda/Simulator.hpp"
#include "sieda/Units.hpp"

namespace sieda {

const char* stageStatusName(StageStatus s) {
    switch (s) {
        case StageStatus::Pass: return "pass";
        case StageStatus::Warning: return "warning";
        case StageStatus::Fail: return "fail";
        case StageStatus::Skipped: return "skipped";
    }
    return "pass";
}

namespace {

StageStatus worst(StageStatus a, StageStatus b) {
    if (a == StageStatus::Skipped) return b;
    if (b == StageStatus::Skipped) return a;
    return static_cast<int>(a) >= static_cast<int>(b) ? a : b;
}

/// Status implied by a list of findings: any error fails, any warning warns.
StageStatus statusOf(const std::vector<RuleViolation>& findings) {
    StageStatus s = StageStatus::Pass;
    for (const auto& v : findings) {
        if (v.severity == Severity::Error) return StageStatus::Fail;
        if (v.severity == Severity::Warning) s = StageStatus::Warning;
    }
    return s;
}

std::string countSummary(const std::vector<RuleViolation>& findings, const std::string& clean) {
    int e = 0, w = 0, i = 0;
    for (const auto& v : findings) (v.severity == Severity::Error ? e : v.severity == Severity::Warning ? w : i)++;
    if (e + w + i == 0) return clean;
    std::ostringstream o;
    o << e << " error" << (e == 1 ? "" : "s") << ", " << w << " warning" << (w == 1 ? "" : "s");
    if (i) o << ", " << i << " note" << (i == 1 ? "" : "s");
    return o.str();
}

RuleViolation finding(Severity s, std::string code, std::string message) {
    RuleViolation v;
    v.severity = s;
    v.code = std::move(code);
    v.message = std::move(message);
    return v;
}

bool endsWith(const std::string& s, const std::string& suffix) {
    return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

int countLines(const std::string& s) { return static_cast<int>(std::count(s.begin(), s.end(), '\n')); }

std::string sizeText(const std::string& content) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.1f KB", static_cast<double>(content.size()) / 1024.0);
    return buf;
}

std::string mdEscape(const std::string& s) {
    std::string out;
    for (char c : s) {
        if (c == '|') out += "\\|";
        else if (c == '\n') out += ' ';
        else out += c;
    }
    return out;
}

}  // namespace

VerificationReport verifyDesign(const Project& project, const VerificationOptions& options) {
    const Schematic& sch = project.schematic;
    const PcbLayout& pcb = project.pcb;
    VerificationReport report;
    report.projectName = project.name;
    report.industry = project.industry;
    if (const IndustryProfile* profile = findIndustry(project.industry)) {
        report.industryName = profile->name;
        report.industryStandards = profile->standards;
    }
    const PartRatings ratings = options.ratings ? *options.ratings : project.partRatings();
    report.rulePreset = pcb.settings.rulePreset;
    report.layerCount = pcb.settings.layerCount;

    int footprinted = 0, placed = 0;
    bool hasSource = false;
    for (const auto& c : sch.components()) {
        if (c.hasFootprint()) {
            ++footprinted;
            placed += c.pcb.placed;
        }
        hasSource |= c.kind == ComponentKind::VoltageSource || c.kind == ComponentKind::CurrentSource;
    }
    const bool empty = sch.components().empty();

    // 1. Electrical rule check.
    {
        VerificationStage st{"erc", "Electrical Rule Check", StageStatus::Pass, "", {}, {}};
        if (empty) {
            st.findings.push_back(finding(Severity::Error, "VER_EMPTY_DESIGN", "The schematic has no components."));
        } else {
            for (auto& v : sch.runERC())
                if (v.code != "ERC_PASS") st.findings.push_back(std::move(v));
        }
        st.status = statusOf(st.findings);
        st.summary = countSummary(st.findings, "No electrical rule violations");
        st.details.push_back(std::to_string(sch.components().size()) + " components, " + std::to_string(sch.nets().size()) +
                             " nets");
        report.stages.push_back(std::move(st));
    }

    // 2. DC operating point.
    {
        VerificationStage st{"simulation", "DC Simulation", StageStatus::Pass, "", {}, {}};
        if (empty) {
            st.status = StageStatus::Skipped;
            st.summary = "Nothing to simulate";
        } else {
            DcResult dc = Simulator(sch).dcOperatingPoint();
            if (!dc.converged) {
                st.findings.push_back(finding(Severity::Error, "VER_DC_NOT_CONVERGED",
                                              "DC operating point did not converge: " + dc.error));
                st.summary = "Did not converge";
            } else {
                st.summary = "Converged in " + std::to_string(dc.iterations) + " iterations";
                if (!hasSource)
                    st.findings.push_back(finding(Severity::Warning, "VER_NO_SOURCE",
                                                  "No voltage or current source: the circuit has no stimulus to simulate."));
                double supply = 0;
                for (const auto& d : dc.devices) {
                    const Component* c = sch.find(d.componentId);
                    if (c && c->kind == ComponentKind::VoltageSource) supply += std::abs(d.power);
                }
                if (hasSource) st.details.push_back("Total supply power " + formatEngineeringValue(supply, "W", 3));
            }
            st.status = statusOf(st.findings);
        }
        report.stages.push_back(std::move(st));
    }

    // 3. Circuit validation (standard values, decoupling, part ratings).
    {
        VerificationStage st{"validation", "Circuit Validation", StageStatus::Pass, "", {}, {}};
        if (empty) {
            st.status = StageStatus::Skipped;
            st.summary = "Nothing to validate";
        } else {
            for (auto& v : validateCircuit(sch, ratings))
                if (v.code != "VAL_DC_FAILED") st.findings.push_back(std::move(v));  // reported by the simulation stage
            st.status = statusOf(st.findings);
            st.summary = countSummary(st.findings, ratings.derating.empty() ? "Values standard, parts within ratings"
                                                                            : "Values standard, parts within derated ratings");
            if (!ratings.derating.empty()) st.details.push_back(ratings.derating);
        }
        report.stages.push_back(std::move(st));
    }

    // 4. Footprint placement.
    {
        VerificationStage st{"placement", "Footprint Placement", StageStatus::Pass, "", {}, {}};
        if (footprinted == 0) {
            st.status = StageStatus::Skipped;
            st.summary = "No parts with footprints";
        } else {
            for (const auto& c : sch.components())
                if (c.hasFootprint() && !c.pcb.placed) {
                    auto v = finding(Severity::Error, "VER_UNPLACED", c.ref + " is not placed on the board.");
                    v.components = {c.id};
                    st.findings.push_back(std::move(v));
                }
            for (auto& v : pcb.runDRC(sch))
                if (v.code == "DRC_OUT_OF_BOARD" || v.code == "DRC_COURTYARD_OVERLAP") st.findings.push_back(std::move(v));
            st.status = statusOf(st.findings);
            st.summary = std::to_string(placed) + " of " + std::to_string(footprinted) + " footprints placed";
            char buf[64];
            std::snprintf(buf, sizeof buf, "Board %.1f × %.1f mm, %d copper layer%s", pcb.settings.width,
                          pcb.settings.height, pcb.settings.layerCount, pcb.settings.layerCount == 1 ? "" : "s");
            st.details.push_back(buf);
        }
        report.stages.push_back(std::move(st));
    }

    // 5. Routing completion.
    {
        VerificationStage st{"routing", "Routing Completion", StageStatus::Pass, "", {}, {}};
        if (placed == 0) {
            st.status = StageStatus::Skipped;
            st.summary = "No placed parts to route";
        } else {
            const size_t open = pcb.ratsnest(sch).size();
            if (open > 0) {
                st.findings.push_back(finding(Severity::Error, "VER_UNROUTED",
                                              std::to_string(open) + " connection" + (open == 1 ? " is" : "s are") +
                                                  " not routed."));
                st.summary = std::to_string(open) + " unrouted connection" + (open == 1 ? "" : "s");
            } else {
                st.summary = "All connections routed";
            }
            double length = 0;
            for (const auto& t : pcb.tracks) length += (t.b - t.a).length();
            char buf[96];
            std::snprintf(buf, sizeof buf, "%zu track segments, %zu vias, %.1f mm of track", pcb.tracks.size(),
                          pcb.vias.size(), length);
            st.details.push_back(buf);
            st.status = statusOf(st.findings);
        }
        report.stages.push_back(std::move(st));
    }

    // 6. Design-rule and fabrication-rule check.
    {
        VerificationStage st{"drc", "Design Rule Check", StageStatus::Pass, "", {}, {}};
        if (placed == 0) {
            st.status = StageStatus::Skipped;
            st.summary = "No board layout";
        } else {
            static const std::set<std::string> covered{"DRC_PASS", "DRC_UNPLACED", "DRC_UNROUTED", "DRC_OUT_OF_BOARD",
                                                       "DRC_COURTYARD_OVERLAP"};
            for (auto& v : pcb.runDRC(sch))
                if (!covered.count(v.code)) st.findings.push_back(std::move(v));
            st.status = statusOf(st.findings);
            st.summary = countSummary(st.findings, "Clean against " + pcb.settings.rulePreset);
            char buf[160];
            std::snprintf(buf, sizeof buf, "Rules: track %.2f mm, clearance %.2f mm, via %.2f/%.2f mm (fab min %.2f/%.2f mm)",
                          pcb.settings.trackWidth, pcb.settings.clearance, pcb.settings.viaDrill, pcb.settings.viaDiameter,
                          pcb.settings.minTrackWidth, pcb.settings.minClearance);
            st.details.push_back(buf);
        }
        report.stages.push_back(std::move(st));
    }

    // 7. Manufacturing outputs: generate every file in memory and check it is complete.
    if (options.includeManufacturing) {
        VerificationStage st{"manufacturing", "Manufacturing Outputs", StageStatus::Pass, "", {}, {}};
        if (placed == 0) {
            st.status = StageStatus::Skipped;
            st.summary = "No board layout to fabricate";
        } else {
            auto pads = pcb.pads(sch);
            int files = 0;
            auto check = [&](const std::string& name, const std::string& content, bool ok, const std::string& problem) {
                ++files;
                st.details.push_back(name + " — " + sizeText(content));
                if (content.empty())
                    st.findings.push_back(finding(Severity::Error, "VER_OUTPUT_EMPTY", name + " is empty."));
                else if (!ok)
                    st.findings.push_back(finding(Severity::Error, "VER_OUTPUT_INVALID", name + ": " + problem));
            };
            const int layers = pcb.settings.layerCount;
            for (int layer = 0; layer < layers; ++layer) {
                std::string g = exportCopperGerber(sch, pcb, layer);
                int features = 0;
                for (const auto& p : pads) features += p.onLayer(layer);
                for (const auto& t : pcb.tracks) features += t.layer == layer;
                if (layers > 1) features += static_cast<int>(pcb.vias.size());
                bool drawn = g.find("D03*") != std::string::npos || g.find("D01*") != std::string::npos;
                check("Copper " + copperLayerName(layer, layers) + " (Gerber)", g, endsWith(g, "M02*\n") && (features == 0 || drawn),
                      features > 0 && !drawn ? "no copper drawn although the layer has pads or tracks" : "missing M02 end of file");
            }
            const std::pair<GerberLayer, const char*> others[] = {{GerberLayer::TopMask, "Solder mask top (Gerber)"},
                                                                  {GerberLayer::BottomMask, "Solder mask bottom (Gerber)"},
                                                                  {GerberLayer::TopSilk, "Silkscreen top (Gerber)"},
                                                                  {GerberLayer::EdgeCuts, "Board outline (Gerber)"}};
            for (const auto& [layer, name] : others) {
                std::string g = exportGerber(sch, pcb, layer);
                bool ok = endsWith(g, "M02*\n") && (layer != GerberLayer::EdgeCuts || g.find("D01*") != std::string::npos);
                check(name, g, ok, layer == GerberLayer::EdgeCuts ? "outline not drawn" : "missing M02 end of file");
            }
            int holes = static_cast<int>(pcb.vias.size());
            for (const auto& p : pads) holes += p.throughHole;
            std::string drill = exportExcellonDrill(sch, pcb);
            check("Drill (Excellon)", drill, endsWith(drill, "M30\n") && (holes == 0 || drill.find("T1") != std::string::npos),
                  "drill file incomplete");
            st.details.back() += ", " + std::to_string(holes) + " holes";
            if (!pcb.settings.holes.empty()) {
                std::string npth = exportExcellonDrill(sch, pcb, false);
                check("Mounting holes (Excellon NPTH)", npth, endsWith(npth, "M30\n") && npth.find("T1") != std::string::npos,
                      "non-plated drill file incomplete");
                st.details.back() += ", " + std::to_string(pcb.settings.holes.size()) + " mounting holes";
            }

            std::string bom = exportBomCsv(sch);
            check("Bill of materials (CSV)", bom, countLines(bom) >= 2, "no parts listed");
            std::string pnp = exportPickAndPlaceCsv(sch);
            check("Pick and place (CSV)", pnp, countLines(pnp) - 1 == placed,
                  std::to_string(countLines(pnp) - 1) + " rows for " + std::to_string(placed) + " placed parts");
            std::string net = exportSpiceNetlist(sch, project.name);
            check("SPICE netlist", net, net.find(".end") != std::string::npos, "missing .end");
            st.status = statusOf(st.findings);
            st.summary = st.findings.empty() ? std::to_string(files) + " files generated and checked"
                                             : countSummary(st.findings, "");
        }
        report.stages.push_back(std::move(st));
    }

    report.verdict = StageStatus::Pass;
    for (const auto& st : report.stages) {
        report.verdict = worst(report.verdict, st.status);
        for (const auto& v : st.findings)
            (v.severity == Severity::Error ? report.errors : v.severity == Severity::Warning ? report.warnings : report.infos)++;
    }
    if (report.verdict == StageStatus::Skipped) report.verdict = StageStatus::Pass;
    return report;
}

Json VerificationReport::toJson() const {
    Json root = Json::object();
    root["project"] = projectName;
    root["industry"] = industry;
    root["industryName"] = industryName;
    root["industryStandards"] = industryStandards;
    root["rulePreset"] = rulePreset;
    root["layerCount"] = layerCount;
    root["verdict"] = stageStatusName(verdict);
    root["passed"] = passed();
    root["errors"] = errors;
    root["warnings"] = warnings;
    root["infos"] = infos;
    Json stagesJson = Json::array();
    for (const auto& st : stages) {
        Json s = Json::object();
        s["id"] = st.id;
        s["title"] = st.title;
        s["status"] = stageStatusName(st.status);
        s["summary"] = st.summary;
        Json details = Json::array();
        for (const auto& d : st.details) details.push(d);
        s["details"] = details;
        s["findings"] = Project::violationsToJson(st.findings);
        stagesJson.push(s);
    }
    root["stages"] = stagesJson;
    root["markdown"] = toMarkdown();
    return root;
}

std::string VerificationReport::toMarkdown() const {
    auto label = [](StageStatus s) {
        switch (s) {
            case StageStatus::Pass: return "PASS";
            case StageStatus::Warning: return "WARNING";
            case StageStatus::Fail: return "FAIL";
            case StageStatus::Skipped: return "SKIPPED";
        }
        return "PASS";
    };
    std::ostringstream o;
    o << "# Design Verification Report\n\n";
    o << "- **Project:** " << mdEscape(projectName) << "\n";
    if (!industryName.empty())
        o << "- **Industry profile:** " << mdEscape(industryName) << " (" << mdEscape(industryStandards) << ")\n";
    o << "- **Design rules:** " << mdEscape(rulePreset) << ", " << layerCount << " copper layer" << (layerCount == 1 ? "" : "s")
      << "\n";
    o << "- **Verdict:** " << label(verdict) << " (" << errors << " errors, " << warnings << " warnings, " << infos
      << " notes)\n\n";
    o << "| Stage | Result | Summary |\n|---|---|---|\n";
    for (const auto& st : stages) o << "| " << st.title << " | " << label(st.status) << " | " << mdEscape(st.summary) << " |\n";
    for (const auto& st : stages) {
        o << "\n## " << st.title << " — " << label(st.status) << "\n\n" << st.summary << "\n";
        if (!st.details.empty()) {
            o << "\n";
            for (const auto& d : st.details) o << "- " << d << "\n";
        }
        if (!st.findings.empty()) {
            o << "\n| Severity | Code | Message |\n|---|---|---|\n";
            for (const auto& v : st.findings)
                o << "| " << severityName(v.severity) << " | `" << v.code << "` | " << mdEscape(v.message) << " |\n";
        }
    }
    o << "\n---\nGenerated by SiEDA design verification.\n";
    return o.str();
}

}  // namespace sieda
