#include "sieda/DeviceModels.hpp"

#include <algorithm>
#include <cctype>

namespace sieda {

namespace {
DeviceModel diode(const char* part, const char* desc, double is, double n, double imax, double pmax, double vmax) {
    DeviceModel m{part, ComponentKind::Diode, desc};
    m.is = is;
    m.emission = n;
    m.maxCurrent = imax;
    m.maxPower = pmax;
    m.maxVoltage = vmax;
    return m;
}
DeviceModel npn(const char* part, const char* desc, double beta, double imax, double pmax, double vmax) {
    DeviceModel m{part, ComponentKind::NPN, desc};
    m.betaF = beta;
    m.maxCurrent = imax;
    m.maxPower = pmax;
    m.maxVoltage = vmax;
    return m;
}
// kp from the datasheet R_DS(on) at a gate drive: R = 1 / (kp · (V_GS − V_th)).
DeviceModel nmos(const char* part, const char* desc, double vth, double rdsOn, double vgs, double imax, double pmax,
                 double vmax) {
    DeviceModel m{part, ComponentKind::NMOS, desc};
    m.vth = vth;
    m.kp = 1.0 / (rdsOn * (vgs - vth));
    m.lambda = 0.01;
    m.maxCurrent = imax;
    m.maxPower = pmax;
    m.maxVoltage = vmax;
    return m;
}

std::vector<DeviceModel> build() {
    return {
        diode("1N4148", "Small-signal switching diode", 2.52e-9, 1.752, 0.3, 0.5, 100),
        diode("1N914", "Small-signal switching diode", 2.52e-9, 1.752, 0.3, 0.5, 100),
        diode("1N4007", "1 A 1000 V rectifier", 7.0e-9, 1.9, 1.0, 1.5, 1000),
        diode("1N4001", "1 A 50 V rectifier", 7.0e-9, 1.9, 1.0, 1.5, 50),
        diode("1N5819", "1 A 40 V Schottky", 3.0e-6, 1.1, 1.0, 1.25, 40),
        diode("SS14", "1 A 40 V Schottky, SMA", 3.0e-6, 1.1, 1.0, 1.25, 40),
        diode("SS34", "3 A 40 V Schottky, SMC", 1.0e-5, 1.1, 3.0, 2.5, 40),
        diode("B5819W", "1 A 40 V Schottky, SOD-123", 3.0e-6, 1.1, 1.0, 0.8, 40),
        diode("BAT54", "200 mA 30 V Schottky", 2.0e-7, 1.05, 0.2, 0.2, 30),
        diode("UF4007", "1 A ultrafast rectifier", 7.0e-9, 1.9, 1.0, 1.5, 1000),
        diode("SMBJ", "TVS diode (forward conduction)", 7.0e-9, 1.9, 2.0, 5.0, 24),
        npn("BC847", "45 V 100 mA NPN, SOT-23", 200, 0.1, 0.25, 45),
        npn("BC547", "45 V 100 mA NPN, TO-92", 200, 0.1, 0.5, 45),
        npn("2N3904", "40 V 200 mA NPN", 150, 0.2, 0.625, 40),
        npn("MMBT3904", "40 V 200 mA NPN, SOT-23", 150, 0.2, 0.35, 40),
        npn("2N2222", "40 V 600 mA NPN", 150, 0.6, 0.5, 40),
        npn("PN2222", "40 V 600 mA NPN", 150, 0.6, 0.625, 40),
        npn("S8050", "25 V 500 mA NPN", 200, 0.5, 0.3, 25),
        npn("BD139", "80 V 1.5 A NPN, TO-126", 100, 1.5, 1.25, 80),
        nmos("2N7002", "60 V 115 mA N-MOSFET, SOT-23", 1.6, 3.45, 4.5, 0.115, 0.2, 60),
        nmos("BSS138", "50 V 200 mA N-MOSFET, SOT-23", 1.3, 3.5, 4.5, 0.2, 0.36, 50),
        nmos("SI2302", "20 V 2.8 A logic-level N-MOSFET, SOT-23", 0.7, 0.085, 2.5, 2.8, 1.25, 20),
        nmos("AO3400", "30 V 5.7 A logic-level N-MOSFET, SOT-23", 0.9, 0.048, 2.5, 5.7, 1.4, 30),
        nmos("IRLML6344", "30 V 5 A logic-level N-MOSFET, SOT-23", 0.8, 0.037, 2.5, 5.0, 1.3, 30),
        nmos("IRLZ44N", "55 V 47 A logic-level N-MOSFET, TO-220", 1.5, 0.025, 5.0, 47, 3.8, 55),
        nmos("IRF540N", "100 V 33 A N-MOSFET, TO-220", 3.0, 0.044, 10.0, 33, 3.8, 100),
        nmos("IRF3205", "55 V 110 A N-MOSFET, TO-220", 3.0, 0.008, 10.0, 110, 3.8, 55),
    };
}

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}
}  // namespace

const std::vector<DeviceModel>& deviceModels() {
    static const std::vector<DeviceModel> models = build();
    return models;
}

const DeviceModel* findDeviceModel(ComponentKind kind, const std::string& value) {
    std::string v = lower(value);
    v.erase(std::remove_if(v.begin(), v.end(), [](char c) { return std::isspace(static_cast<unsigned char>(c)); }), v.end());
    if (v.empty()) return nullptr;
    const DeviceModel* best = nullptr;
    for (const auto& m : deviceModels()) {
        if (m.kind != kind) continue;
        std::string p = lower(m.part);
        // "SI2302", "SI2302CDS", "si2302-t1": the value starts with the part number.
        if (v.compare(0, p.size(), p) == 0 && (!best || p.size() > best->part.size())) best = &m;
    }
    return best;
}

}  // namespace sieda
