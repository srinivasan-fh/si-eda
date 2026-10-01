import SwiftUI

/// Design Checks workspace: the one-click verification sign-off plus the individual ERC, circuit-validation and DRC
/// results (Altium "Messages" panel style).
struct RuleCheckView: View {
    @EnvironmentObject private var store: DesignStore
    @State private var filter: ViolationSeverity? = nil

    private var mode: ChecksMode { store.checksMode }

    var body: some View {
        VStack(spacing: 0) {
            OptionsBar {
                Image(systemName: "checkmark.seal").foregroundStyle(Theme.blue)
                Text("Design Checks").fontWeight(.semibold).foregroundStyle(Theme.textPrimary)
                Picker("Mode", selection: $store.checksMode) {
                    ForEach(ChecksMode.allCases) { Text($0.rawValue).tag($0) }
                }
                .pickerStyle(.segmented)
                .labelsHidden()
                .frame(width: 220)
                Divider().frame(height: 18)
                if mode == .verification {
                    Button { Task { await store.runVerification() } } label: {
                        Label("Verify Design", systemImage: "checkmark.shield")
                    }
                    .disabled(store.isBusy)
                    Button { store.exportVerificationReport() } label: {
                        Label("Export Report…", systemImage: "square.and.arrow.up")
                    }
                    .disabled(store.verificationReport == nil)
                    Spacer()
                } else {
                    Button { store.runERC() } label: { Label("ERC", systemImage: "bolt.badge.checkmark") }
                    Button { store.runValidation() } label: { Label("Validate", systemImage: "checklist") }
                    Button { store.runDRC() } label: { Label("DRC", systemImage: "square.grid.3x3.square") }
                    Spacer()
                    Picker("Show", selection: $filter) {
                        Text("All").tag(ViolationSeverity?.none)
                        Text("Errors").tag(ViolationSeverity?.some(.error))
                        Text("Warnings").tag(ViolationSeverity?.some(.warning))
                        Text("Info").tag(ViolationSeverity?.some(.info))
                    }
                    .pickerStyle(.segmented)
                    .labelsHidden()
                    .frame(width: 280)
                }
            }
            .buttonStyle(.borderless)

            switch mode {
            case .verification:
                VerificationPanel()
            case .rules:
                HStack(alignment: .top, spacing: 12) {
                    ViolationColumn(title: "Electrical Rule Check", icon: "bolt.shield", results: store.ercResults,
                                    filter: filter, target: .schematic)
                    ViolationColumn(title: "Circuit Validation", icon: "checklist", results: store.validationResults,
                                    filter: filter, target: .schematic)
                    ViolationColumn(title: "Design Rule Check", icon: "square.grid.3x3.square", results: store.drcResults,
                                    filter: filter, target: .pcb, subtitle: store.snapshot.board.rulePreset)
                }
                .padding(12)
            }
        }
        .background(Theme.navy)
        // `.task` runs after the workspace switch has been laid out (publishing results during it is reentrant).
        .task {
            if store.ercResults.isEmpty { store.runERC() }
            if store.validationResults.isEmpty, !store.snapshot.components.isEmpty { store.runValidation() }
            if store.drcResults.isEmpty, !store.snapshot.pads.isEmpty { store.runDRC() }
        }
    }
}

// MARK: - Verification

/// Stage-by-stage sign-off: ERC → DC → validation → placement → routing → DRC → manufacturing outputs.
private struct VerificationPanel: View {
    @EnvironmentObject private var store: DesignStore

    var body: some View {
        if let report = store.verificationReport {
            ScrollView {
                VStack(alignment: .leading, spacing: 12) {
                    VerdictHeader(report: report, stale: store.verificationIsStale)
                    ForEach(Array(report.stages.enumerated()), id: \.element.id) { index, stage in
                        StageCard(number: index + 1, stage: stage)
                    }
                }
                .padding(12)
            }
        } else {
            BlueEmptyState(systemImage: "checkmark.shield",
                           title: "Verify the design before fabrication",
                           message: "One pass runs every built-in check: electrical rules, DC simulation, circuit validation (standard values, decoupling, part ratings derated for the industry profile), footprint placement, routing completion, the \(store.snapshot.board.rulePreset) design rules and the manufacturing outputs. The fabrication export runs it too.",
                           actionTitle: "Verify Design") { Task { await store.runVerification() } }
                .frame(maxWidth: .infinity, maxHeight: .infinity)
        }
    }
}

private struct VerdictHeader: View {
    let report: VerificationReport
    let stale: Bool
    @EnvironmentObject private var store: DesignStore

    var body: some View {
        HStack(spacing: 14) {
            Image(systemName: report.verdict.systemImage)
                .font(.system(size: 34, weight: .medium))
                .foregroundStyle(statusColor(report.verdict))
                .accessibilityHidden(true)
            VStack(alignment: .leading, spacing: 4) {
                Text(report.verdict == .fail ? "Not ready for fabrication" : "Ready for fabrication")
                    .font(.title3.weight(.semibold))
                    .foregroundStyle(Theme.textPrimary)
                Text("\(report.verdict.title) · \(report.errors) errors · \(report.warnings) warnings · \(report.infos) notes")
                    .foregroundStyle(Theme.textSecondary)
                Text("\(report.industryName.isEmpty ? "" : report.industryName + " · ")\(report.rulePreset) · \(report.layerCount) copper layer\(report.layerCount == 1 ? "" : "s")")
                    .font(.caption)
                    .foregroundStyle(Theme.textMuted)
            }
            Spacer()
            if stale {
                HStack(spacing: 8) {
                    Label("Design changed since this run", systemImage: "clock.arrow.circlepath")
                        .foregroundStyle(Theme.warning)
                    Button("Re-verify") { Task { await store.runVerification() } }
                        .buttonStyle(.borderedProminent)
                        .controlSize(.small)
                }
                .font(.callout)
            }
        }
        .padding(14)
        .frame(maxWidth: .infinity, alignment: .leading)
        .bluePanel()
        .accessibilityElement(children: .combine)
    }
}

private struct StageCard: View {
    let number: Int
    let stage: VerificationStage
    @EnvironmentObject private var store: DesignStore
    @State private var expanded = false

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            Button {
                withAnimation(.easeInOut(duration: 0.15)) { expanded.toggle() }
            } label: {
                HStack(spacing: 10) {
                    Text("\(number)")
                        .font(.caption.monospacedDigit().weight(.bold))
                        .foregroundStyle(Theme.skyBlue)
                        .frame(width: 22, height: 22)
                        .background(Circle().fill(Theme.blue.opacity(0.2)))
                    Image(systemName: stage.systemImage).foregroundStyle(Theme.lightBlue).frame(width: 20)
                    VStack(alignment: .leading, spacing: 2) {
                        Text(stage.title).font(.headline).foregroundStyle(Theme.textPrimary)
                        Text(stage.summary).font(.callout).foregroundStyle(Theme.textSecondary)
                    }
                    Spacer()
                    Label(stage.status.title, systemImage: stage.status.systemImage)
                        .font(.callout.weight(.semibold))
                        .foregroundStyle(statusColor(stage.status))
                    Image(systemName: expanded ? "chevron.down" : "chevron.right")
                        .foregroundStyle(Theme.textMuted)
                        .opacity(stage.details.isEmpty && stage.findings.isEmpty ? 0 : 1)
                }
                .contentShape(Rectangle())
            }
            .buttonStyle(.plain)
            .disabled(stage.details.isEmpty && stage.findings.isEmpty)
            .accessibilityLabel("\(stage.title): \(stage.status.title). \(stage.summary)")

            if expanded {
                VStack(alignment: .leading, spacing: 6) {
                    ForEach(stage.details, id: \.self) { detail in
                        Label(detail, systemImage: "info.circle")
                            .font(.caption)
                            .foregroundStyle(Theme.textMuted)
                    }
                    ForEach(stage.findings.sorted { $0.severity > $1.severity }) { finding in
                        ViolationRow(violation: finding, target: stage.workspace)
                    }
                }
                .padding(.leading, 32)
            }
        }
        .padding(12)
        .frame(maxWidth: .infinity, alignment: .leading)
        .bluePanel()
        .onAppear { expanded = stage.status == .fail || stage.status == .warning }
    }
}

private func statusColor(_ status: VerificationStatus) -> Color {
    switch status {
    case .pass: return Theme.skyBlue
    case .warning: return Theme.warning
    case .fail: return Theme.error
    case .skipped: return Theme.textMuted
    }
}

// MARK: - Individual checks

private struct ViolationColumn: View {
    var title: String
    var icon: String
    var results: [RuleViolation]
    var filter: ViolationSeverity?
    var target: Workspace
    var subtitle: String?

    var body: some View {
        let visible = results.filter { filter == nil || $0.severity == filter }.sorted { $0.severity > $1.severity }
        let errors = results.filter { $0.severity == .error }.count
        let warnings = results.filter { $0.severity == .warning }.count
        VStack(alignment: .leading, spacing: 10) {
            HStack {
                VStack(alignment: .leading, spacing: 2) {
                    Label(title, systemImage: icon).font(.headline).foregroundStyle(Theme.textPrimary)
                    if let subtitle {
                        Text(subtitle).font(.caption).foregroundStyle(Theme.textMuted)
                    }
                }
                Spacer()
                Badge(text: "\(errors) E · \(warnings) W",
                      systemImage: errors == 0 ? "checkmark.circle" : "xmark.octagon")
            }
            if visible.isEmpty {
                Text(results.isEmpty ? "Not run yet." : "Nothing to show for this filter.")
                    .foregroundStyle(Theme.textMuted)
                    .padding(.vertical, 20)
                Spacer()
            } else {
                ScrollView {
                    LazyVStack(alignment: .leading, spacing: 6) {
                        ForEach(visible) { v in ViolationRow(violation: v, target: target) }
                    }
                }
            }
        }
        .padding(12)
        .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .topLeading)
        .bluePanel()
    }
}

/// One finding; clicking it selects the affected parts and jumps to the editor that shows them.
private struct ViolationRow: View {
    let violation: RuleViolation
    let target: Workspace
    @EnvironmentObject private var store: DesignStore

    var body: some View {
        Button {
            if !violation.components.isEmpty {
                store.selection = Set(violation.components)
                store.selectedWire = nil
                store.workspace = target
            }
        } label: {
            HStack(alignment: .top, spacing: 10) {
                Image(systemName: Theme.severityIcon(violation.severity))
                    .foregroundStyle(Theme.severityColor(violation.severity))
                VStack(alignment: .leading, spacing: 2) {
                    Text(violation.message).foregroundStyle(Theme.textPrimary).multilineTextAlignment(.leading)
                    Text(violation.code).font(.caption.monospaced()).foregroundStyle(Theme.textMuted)
                }
                Spacer()
                if !violation.components.isEmpty {
                    Image(systemName: "scope").foregroundStyle(Theme.lightBlue)
                }
            }
            .padding(8)
            .background(RoundedRectangle(cornerRadius: 8).fill(Theme.deepBlue.opacity(0.7)))
            .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
        .help(violation.components.isEmpty ? "" : "Cross-probe to the affected parts")
    }
}
