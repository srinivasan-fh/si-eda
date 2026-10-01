import Foundation

/// Mirror of `sieda::ComponentKind` in the C++ core. Raw values must match `Library.hpp`.
enum ComponentKind: Int, CaseIterable, Identifiable, Codable {
    case resistor = 0
    case capacitor = 1
    case inductor = 2
    case diode = 3
    case led = 4
    case voltageSource = 5
    case currentSource = 6
    case ground = 7
    case npn = 8
    case nmos = 9
    case opAmp = 10
    case switchSPST = 11
    case connector = 12
    case ic8 = 13
    case fuse = 14
    case netLabel = 15
    case custom = 16  // user-defined part (datasheet import); see CustomPartInfo

    var id: Int { rawValue }

    /// Kinds offered in the built-in device picker (custom parts are listed from the project library).
    static var builtIn: [ComponentKind] { allCases.filter { $0 != .custom } }

    var displayName: String {
        switch self {
        case .resistor: return "Resistor"
        case .capacitor: return "Capacitor"
        case .inductor: return "Inductor"
        case .diode: return "Diode"
        case .led: return "LED"
        case .voltageSource: return "Voltage Source"
        case .currentSource: return "Current Source"
        case .ground: return "Ground"
        case .npn: return "NPN Transistor"
        case .nmos: return "N-MOSFET"
        case .opAmp: return "Op-Amp"
        case .switchSPST: return "Switch"
        case .connector: return "Connector"
        case .ic8: return "IC (8-pin)"
        case .fuse: return "Fuse"
        case .netLabel: return "Net Label"
        case .custom: return "Custom Part"
        }
    }

    /// Identifier used in AI design plans (`DesignPlan.components[].kind`).
    var planName: String {
        switch self {
        case .resistor: return "resistor"
        case .capacitor: return "capacitor"
        case .inductor: return "inductor"
        case .diode: return "diode"
        case .led: return "led"
        case .voltageSource: return "voltage_source"
        case .currentSource: return "current_source"
        case .ground: return "ground"
        case .npn: return "npn"
        case .nmos: return "nmos"
        case .opAmp: return "opamp"
        case .switchSPST: return "switch"
        case .connector: return "connector"
        case .ic8: return "ic8"
        case .fuse: return "fuse"
        case .netLabel: return "net_label"
        case .custom: return "custom"
        }
    }

    var defaultValue: String {
        switch self {
        case .resistor: return "10k"
        case .capacitor: return "100n"
        case .inductor: return "10u"
        case .diode: return "1N4148"
        case .led: return "Red"
        case .voltageSource: return "5"
        case .currentSource: return "1m"
        case .ground: return "0"
        case .npn: return "BC847"
        case .nmos: return "2N7002"
        case .opAmp: return "LM358"
        case .switchSPST: return "on"
        case .connector: return "Conn_01x02"
        case .ic8: return "IC"
        case .fuse: return "500m"
        case .netLabel: return "VCC"
        case .custom: return ""
        }
    }

    /// Pin names in core order (index = pin index).
    var pinNames: [String] {
        switch self {
        case .resistor, .capacitor, .inductor, .switchSPST, .fuse, .connector: return ["1", "2"]
        case .diode, .led: return ["A", "K"]
        case .voltageSource, .currentSource: return ["+", "-"]
        case .ground: return ["GND"]
        case .npn: return ["B", "C", "E"]
        case .nmos: return ["G", "D", "S"]
        case .opAmp: return ["IN+", "IN-", "OUT"]
        case .ic8: return ["1", "2", "3", "4", "5", "6", "7", "8"]
        case .netLabel: return ["N"]
        case .custom: return []  // defined per part
        }
    }

    var systemImage: String {
        switch self {
        case .resistor: return "rectangle.and.hand.point.up.left"
        case .capacitor: return "pause"
        case .inductor: return "wave.3.right"
        case .diode: return "arrowtriangle.right"
        case .led: return "lightbulb"
        case .voltageSource: return "bolt.circle"
        case .currentSource: return "arrow.up.circle"
        case .ground: return "arrow.down.to.line"
        case .npn: return "triangle"
        case .nmos: return "switch.2"
        case .opAmp: return "play"
        case .switchSPST: return "power"
        case .connector: return "cable.connector"
        case .ic8: return "cpu"
        case .fuse: return "minus.rectangle"
        case .netLabel: return "tag"
        case .custom: return "cpu.fill"
        }
    }

    var category: String {
        switch self {
        case .resistor, .capacitor, .inductor, .fuse: return "Passives"
        case .diode, .led, .npn, .nmos, .opAmp, .ic8: return "Semiconductors"
        case .voltageSource, .currentSource, .ground, .netLabel: return "Power & Nets"
        case .switchSPST, .connector: return "Electromechanical"
        case .custom: return "Custom Parts"
        }
    }

    /// Short guidance used both in the UI and in agent prompts.
    var valueHint: String {
        switch self {
        case .resistor: return "Resistance, e.g. 330, 4k7, 10k, 1M"
        case .capacitor: return "Capacitance, e.g. 100n, 10u"
        case .inductor: return "Inductance, e.g. 10u, 1m"
        case .diode: return "Part number, e.g. 1N4148"
        case .led: return "Colour: Red, Green, Yellow, Blue, White"
        case .voltageSource: return "DC volts (5), SIN(off amp freq) or PULSE(v1 v2 period [duty])"
        case .currentSource: return "Amps leaving the + pin, e.g. 1m"
        case .ground: return "—"
        case .npn: return "Part number, e.g. BC847, 2N3904"
        case .nmos: return "Part number, e.g. 2N7002"
        case .opAmp: return "Part number, e.g. LM358 (ideal model, ±15 V rails)"
        case .switchSPST: return "on / off"
        case .connector: return "Description"
        case .ic8: return "Part number (no simulation model)"
        case .fuse: return "Rating, e.g. 500m"
        case .netLabel: return "Net name; identical names connect (GND joins ground)"
        case .custom: return "Part number or value"
        }
    }

    /// Kinds without a PCB footprint.
    var isVirtual: Bool { self == .ground || self == .netLabel }

    /// Accepts plan names plus common aliases an LLM might produce.
    static func fromPlanName(_ raw: String) -> ComponentKind? {
        let key = raw.lowercased()
            .replacingOccurrences(of: "-", with: "_")
            .replacingOccurrences(of: " ", with: "_")
        if let exact = ComponentKind.allCases.first(where: { $0.planName == key }) { return exact }
        switch key {
        case "r", "res": return .resistor
        case "c", "cap": return .capacitor
        case "l", "coil": return .inductor
        case "d", "rectifier": return .diode
        case "light_emitting_diode": return .led
        case "v", "vsource", "dc_source", "battery", "power", "power_supply", "voltage": return .voltageSource
        case "i", "isource", "current": return .currentSource
        case "gnd", "earth": return .ground
        case "bjt", "transistor", "npn_transistor": return .npn
        case "mosfet", "nfet", "n_mosfet", "nmos_transistor": return .nmos
        case "op_amp", "operational_amplifier", "amplifier": return .opAmp
        case "sw", "spst", "button", "push_button": return .switchSPST
        case "conn", "header", "jack": return .connector
        case "ic", "chip", "soic8", "dip8": return .ic8
        case "f": return .fuse
        case "label", "net", "netlabel", "power_label": return .netLabel
        default: return nil
        }
    }
}
