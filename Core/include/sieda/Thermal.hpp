// SiEDA Core — board thermal analysis: the steady-state temperature over the board from each part's dissipation.
// The board is a conducting sheet on a grid of square cells: the laminate plus every copper layer's coverage in the
// cell (tracks, pads, pours) spread the heat sideways, both faces lose it to the air (convection + radiation, a film
// coefficient per face). Each part's power enters through its pads. The sheet is taken isothermal through its
// thickness (a first-order board model: thermal vias are not modelled apart from the copper they carry).
#pragma once

#include <map>
#include <string>
#include <vector>

#include "sieda/Json.hpp"
#include "sieda/Project.hpp"

namespace sieda {

struct ThermalOptions {
    double ambientC = 25;
    double hTop = 10, hBottom = 10;     // W/m²·K per face: still air ≈ 10 (convection + radiation), fan ≈ 25–50
    double cell = 0;                    // mm; 0 = chosen from the board size (at most ~20 000 cells)
    std::map<std::string, double> power;   // designator → W: replaces the DC operating point's dissipation
    std::map<std::string, double> thetaJB; // designator → °C/W junction-to-board (else estimated from the package)
    std::map<std::string, double> heatsink; // designator → °C/W junction to ambient through its heatsink (θJC + θCS + θSA)
    double junctionLimitC = 125;        // THERMAL_JUNCTION above this
};

struct ThermalPart {
    int componentId = -1;
    std::string ref;
    double power = 0;          // W
    std::string powerSource;   // "set" (options) or "dc" (DC operating point)
    double boardC = 0;         // hottest board cell under its pads
    double junctionC = 0;      // boardC + (power − toAirW) × θJB
    double thetaJB = 0;
    double thetaHeatsink = 0;  // °C/W, 0 = none
    double thetaBody = 0;      // °C/W from its body's top and sides to the air (footprint body size), 0 = none
    double toAirW = 0;         // the share of its power leaving straight to the air (body + heatsink), not through the board
    bool thetaEstimated = true;
    Vec2 position;
};

struct ThermalFinding {
    std::string code, severity, message, ref;
};

struct ThermalResult {
    double ambientC = 25, maxC = 25, cell = 1, x0 = 0, y0 = 0, totalPower = 0;
    int nx = 0, ny = 0, iterations = 0;
    bool converged = false;
    Vec2 hottest;
    std::vector<double> temperature;  // nx × ny, row by row from y0; NaN outside the board
    std::vector<ThermalPart> parts;   // parts with power, hottest junction first
    std::vector<ThermalFinding> findings;
    std::vector<std::string> notes;
};

ThermalResult thermalAnalysis(const Project& project, const ThermalOptions& options = {});
/// Options from JSON: {ambient, hTop, hBottom, cell, power:{REF:W}, thetaJB:{REF:°C/W}, heatsink:{REF:°C/W}, junctionLimit}.
ThermalOptions thermalOptionsFromJson(const Json& j);
/// {ambientC, maxC, hottest:{x,y}, totalPowerW, converged, iterations, parts:[…], findings:[…], notes:[…]} and, with
/// `withMap`, map:{x0, y0, cell, nx, ny, t:[°C or null, row by row]}.
Json thermalJson(const ThermalResult& r, bool withMap);

}  // namespace sieda
