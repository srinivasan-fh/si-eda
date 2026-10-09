// sieda-cli — headless SiEDA pipeline: ERC → DC simulation → placement → routing → DRC → verification → fabrication outputs.
//
//   sieda-cli                       run the built-in demo design
//   sieda-cli --demo out/           run the demo and write its fabrication files into out/
//   sieda-cli design.siedaproj      process a saved project
//   sieda-cli design.siedaproj out/ write Gerbers, drill, BOM, netlist, STL into out/
//   sieda-cli --chat model.gguf "question"   ask the built-in language model (docs/AI.md), the answer streamed
#include <cstdio>
#include <fstream>
#include <iostream>
#include <sstream>

#include "sieda/Export.hpp"
#include "sieda/Fabrication.hpp"
#include "sieda/LocalModel.hpp"
#include "sieda/Reliability.hpp"
#include "sieda/Mesh.hpp"
#include "sieda/Project.hpp"
#include "sieda/Units.hpp"
#include "sieda/Verification.hpp"

using namespace sieda;

namespace {
Project demoProject() {
    Project p;
    p.name = "Demo: LED indicator with transistor driver";
    p.requirements = "5 V input, logic-level input switches an LED through an NPN transistor.";
    auto& s = p.schematic;
    int j = s.addComponent(ComponentKind::VoltageSource, "5", {0, 0});
    int g = s.addComponent(ComponentKind::Ground, "", {0, 80});
    int rb = s.addComponent(ComponentKind::Resistor, "10k", {100, 0});
    int rc = s.addComponent(ComponentKind::Resistor, "330", {200, -80});
    int d = s.addComponent(ComponentKind::LED, "Red", {280, -80});
    int q = s.addComponent(ComponentKind::NPN, "BC847", {200, 0});
    auto pin = [&](int c, const char* n) { return PinRef{c, s.pinIndex(c, n)}; };
    s.connect(pin(j, "+"), pin(rb, "1"));
    s.connect(pin(rb, "2"), pin(q, "B"));
    s.connect(pin(j, "+"), pin(rc, "1"));
    s.connect(pin(rc, "2"), pin(d, "A"));
    s.connect(pin(d, "K"), pin(q, "C"));
    s.connect(pin(q, "E"), pin(g, "GND"));
    s.connect(pin(j, "-"), pin(g, "GND"));
    p.pcb.settings.width = 30;
    p.pcb.settings.height = 22;
    p.schematicChanged();
    return p;
}

void printViolations(const char* title, const std::vector<RuleViolation>& list) {
    std::printf("\n== %s ==\n", title);
    for (const auto& v : list) std::printf("  [%-7s] %-24s %s\n", severityName(v.severity), v.code.c_str(), v.message.c_str());
}

bool writeFile(const std::string& path, const std::string& content) {
    std::ofstream f(path, std::ios::binary);
    f << content;
    return static_cast<bool>(f);
}
}  // namespace

int main(int argc, char** argv) {
    if (argc >= 4 && std::string(argv[1]) == "--chat") {
        LocalModel model;
        std::string error;
        if (!model.load(argv[2], &error)) {
            std::fprintf(stderr, "%s\n", error.c_str());
            return 1;
        }
        std::fprintf(stderr, "%s\n", model.infoJson().c_str());
        try {
            model.generate(model.chatPrompt("You are an electronics design assistant.", argv[3]), LocalModel::Options(), [](const std::string& s) {
                std::fputs(s.c_str(), stdout);
                std::fflush(stdout);
                return true;
            });
        } catch (const std::exception& e) {
            std::fprintf(stderr, "%s\n", e.what());
            return 1;
        }
        std::printf("\n");
        return 0;
    }
    Project project;
    if (argc >= 2 && std::string(argv[1]) != "--demo") {
        std::ifstream in(argv[1]);
        if (!in) {
            std::fprintf(stderr, "cannot open %s\n", argv[1]);
            return 2;
        }
        std::stringstream ss;
        ss << in.rdbuf();
        try {
            project = Project::fromJson(Json::parse(ss.str()));
        } catch (const std::exception& e) {
            std::fprintf(stderr, "invalid project: %s\n", e.what());
            return 2;
        }
    } else {
        project = demoProject();
    }

    std::printf("SiEDA CLI — %s\n", project.name.c_str());
    std::printf("%zu components, %zu wires, %zu nets\n", project.schematic.components().size(),
                project.schematic.wires().size(), project.schematic.nets().size());

    printViolations("Electrical Rule Check", project.schematic.runERC());

    Simulator sim(project.schematic);
    DcResult dc = sim.dcOperatingPoint();
    std::printf("\n== DC operating point (%s, %d iterations) ==\n", dc.converged ? "converged" : dc.error.c_str(),
                dc.iterations);
    if (dc.converged) {
        const auto& nets = project.schematic.nets();
        for (size_t i = 0; i < nets.size(); ++i)
            if (nets[i].pins.size() > 1)
                std::printf("  V(%-8s) = %s\n", nets[i].name.c_str(), formatEngineeringValue(dc.netVoltages[i], "V", 4).c_str());
        for (const auto& d : dc.devices) {
            const Component* c = project.schematic.find(d.componentId);
            std::printf("  %-5s I = %-10s P = %s\n", c->ref.c_str(), formatEngineeringValue(d.current, "A", 4).c_str(),
                        formatEngineeringValue(d.power, "W", 3).c_str());
        }
    }

    project.pcb.autoPlace(project.schematic, false);
    RouteStats st = project.autoRoute();  // with the strategy's pin / gate swap first when it is on
    std::printf("\n== Autorouter ==\n  %d/%d connections routed, %d vias, %.1f mm of track\n", st.routed, st.connections,
                st.vias, st.trackLength);
    auto drc = project.pcb.runDRC(project.schematic);
    printViolations("Design Rule Check", drc);
    printViolations("Design for Reliability", reliabilityChecks(project));

    VerificationReport verification = verifyDesign(project);
    std::printf("\n== Design verification: %s ==\n", stageStatusName(verification.verdict));
    for (const auto& st : verification.stages)
        std::printf("  %-24s %-8s %s\n", st.title.c_str(), stageStatusName(st.status), st.summary.c_str());

    if (argc >= 3) {
        std::string dir = argv[2];
        if (!dir.empty() && dir.back() != '/') dir += '/';
        std::vector<std::string> written;
        std::string error;
        bool ok = writeFabricationPackage(project, dir, &written, &error);
        ok &= writeFile(dir + "design.siedaproj", project.toJson().dump(true));
        ok &= writeFile(dir + "verification_report.md", verification.toMarkdown());
        for (const auto& f : written) std::printf("  %s\n", f.c_str());
        if (!error.empty()) std::printf("  error: %s\n", error.c_str());
        std::printf("\n%s the fabrication package (%zu files) to %s\n", ok ? "Wrote" : "FAILED writing",
                    written.size() + 2, dir.c_str());
        if (!ok) return 1;
    }
    return verification.passed() ? 0 : 1;
}
