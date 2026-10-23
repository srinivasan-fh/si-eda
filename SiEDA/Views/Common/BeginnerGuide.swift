// SiEDA — Beginner mode (Settings → General): a bar over the workspace that names the next step of a board, from parts
// to an order, with one button for it; the Library workspace (symbol / footprint / 3D model editors) is hidden.
import SwiftUI

enum BeginnerMode {
    static let key = "ui.beginner"
    static var isOn: Bool { UserDefaults.standard.bool(forKey: key) }
}

struct BeginnerGuide: View {
    @EnvironmentObject private var store: DesignStore

    private struct Step {
        let number: Int
        let title: LocalizedStringKey
        let hint: LocalizedStringKey
        let action: LocalizedStringKey
        let run: () -> Void
    }

    /// The first step the design has not done yet.
    private var step: Step {
        let s = store.snapshot
        if s.components.isEmpty {
            return Step(number: 1, title: "Add parts", hint: "Pick parts from the left of the schematic and wire their pins.",
                        action: "Open Schematic") { store.workspace = .schematic }
        }
        if s.courtyards.isEmpty {
            return Step(number: 2, title: "Bring the parts onto the board",
                        hint: "In the schematic, Update PCB carries the parts and their connections to the board.",
                        action: "Open Schematic") { store.workspace = .schematic }
        }
        if !s.ratsnest.isEmpty {
            return Step(number: 3, title: "Place and route", hint: "Auto Place arranges the parts; Auto Route draws the copper tracks.",
                        action: "Auto Place and Route") {
                store.workspace = .pcb
                Task {
                    await store.autoPlaceInBackground(all: true)
                    await store.autoRouteBoard()
                }
            }
        }
        return Step(number: 4, title: "Check and order", hint: "Verify the design, then order the boards with the fabrication package.",
                    action: "Verify Design") {
            Task {
                await store.runVerification()
                store.showChecks(.verification)
            }
        }
    }

    var body: some View {
        let step = self.step
        HStack(spacing: 10) {
            Image(systemName: "graduationcap").foregroundStyle(Theme.skyBlue)
            Text(verbatim: "\(step.number) / 4").font(.caption.monospacedDigit()).foregroundStyle(Theme.textMuted)
            Text(step.title).font(.callout.weight(.semibold)).foregroundStyle(Theme.textPrimary)
            Text(step.hint).font(.caption).foregroundStyle(Theme.textSecondary).lineLimit(1)
            Spacer()
            Button(step.action, action: step.run).controlSize(.small)
            if step.number == 4 { Button("Order Boards…") { store.showFabOrder = true }.controlSize(.small) }
        }
        .padding(.horizontal, 12)
        .padding(.vertical, 6)
        .background(Theme.deepBlue)
    }
}
