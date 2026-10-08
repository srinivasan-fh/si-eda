import Foundation
import os
import SwiftUI

/// One edit's cost in milliseconds (Help → Performance…): the undo state, the engine operation and the view refresh.
struct EditTiming: Identifiable, Equatable {
    let id = UUID()
    let action: String
    let stateMs, engineMs, refreshMs: Double
    var totalMs: Double { stateMs + engineMs + refreshMs }
}

enum PerformanceLog {
    /// Instruments (os_signpost): every edit is an interval named "edit" with the action as its message.
    static let signposter = OSSignposter(subsystem: "app.sieda.SiEDA", category: .pointsOfInterest)
    static let capacity = 200

    static func milliseconds(_ from: DispatchTime, _ to: DispatchTime) -> Double {
        Double(to.uptimeNanoseconds - from.uptimeNanoseconds) / 1e6
    }

    /// The `p` quantile (0…1) of `values`, nearest rank; 0 when empty.
    static func percentile(_ values: [Double], _ p: Double) -> Double {
        guard !values.isEmpty else { return 0 }
        let sorted = values.sorted()
        return sorted[min(sorted.count - 1, max(0, Int((p * Double(sorted.count)).rounded(.up)) - 1))]
    }
}

/// Help → Performance…: how long recent edits took, split into undo state, engine and view refresh.
struct PerformanceView: View {
    @EnvironmentObject private var store: DesignStore
    @Environment(\.dismiss) private var dismiss

    var body: some View {
        let timings = store.editTimings
        let totals = timings.map(\.totalMs)
        VStack(alignment: .leading, spacing: 10) {
            HStack {
                Text("Performance").font(.title3.bold()).foregroundStyle(Theme.textPrimary)
                Spacer()
                Button("Done") { dismiss() }.keyboardShortcut(.defaultAction)
            }
            HStack(spacing: 18) {
                stat("Median", PerformanceLog.percentile(totals, 0.5))
                stat("95th percentile", PerformanceLog.percentile(totals, 0.95))
                stat("Slowest", totals.max() ?? 0)
            }
            Table(timings.reversed()) {
                TableColumn("Edit", value: \.action)
                TableColumn("Total") { Text(verbatim: ms($0.totalMs)).monospacedDigit() }
                TableColumn("Undo state") { Text(verbatim: ms($0.stateMs)).monospacedDigit() }
                TableColumn("Engine") { Text(verbatim: ms($0.engineMs)).monospacedDigit() }
                TableColumn("View refresh") { Text(verbatim: ms($0.refreshMs)).monospacedDigit() }
            }
        }
        .padding(16)
        .frame(width: 620, height: 460)
        .background(Theme.deepBlue)
    }

    private func ms(_ v: Double) -> String { String(format: "%.1f ms", v) }

    private func stat(_ title: LocalizedStringKey, _ value: Double) -> some View {
        VStack(alignment: .leading) {
            Text(title).font(.caption).foregroundStyle(Theme.textMuted)
            Text(verbatim: ms(value)).font(.title3.monospacedDigit()).foregroundStyle(Theme.skyBlue)
        }
    }
}
