import AppKit
import SwiftUI
import UniformTypeIdentifiers

/// Prompt / PRD driven design: the user describes the product, the agent team designs, verifies and lays it out.
struct PromptStudioView: View {
    @EnvironmentObject private var store: DesignStore
    @EnvironmentObject private var settings: AISettings
    @EnvironmentObject private var agents: AgentOrchestrator
    @AppStorage("promptStudio.brief") private var brief = ""
    @State private var refinement = ""

    struct Example: Identifiable {
        var title: String
        var icon: String
        var prompt: String
        var id: String { title }
    }

    static let examples: [Example] = [
        Example(title: "Status LED", icon: "lightbulb",
                prompt: "A 5 V USB-powered status indicator: one red LED at about 10 mA, smallest possible two-layer board."),
        Example(title: "Sensor amplifier", icon: "waveform.path",
                prompt: "Amplify a 100 mV, 1 kHz sensor signal with a gain of 11 using a single op-amp. Provide a labelled VOUT net."),
        Example(title: "Transistor driver", icon: "switch.2",
                prompt: "Let a push-button switch an LED through an NPN transistor from a 5 V rail; keep the base current under 1 mA."),
        Example(title: "PWM load switch", icon: "bolt.car",
                prompt: "Drive a 12 V, 100 mA resistive load with a 100 Hz PWM signal using an N-channel MOSFET low-side switch with a gate pull-down."),
        Example(title: "Anti-alias filter", icon: "line.3.horizontal.decrease",
                prompt: "First-order RC low-pass filter with a cutoff near 1.6 kHz for a 1 kHz test signal."),
        Example(title: "Reference divider", icon: "divide",
                prompt: "Derive a 6 V reference from a 12 V supply with a resistive divider and expose it as VOUT."),
        Example(title: "5 V regulator", icon: "bolt.batteryblock",
                prompt: "Regulate a 12 V input down to a 5 V rail with an LM7805, datasheet input/output capacitors and a power-good LED."),
        Example(title: "555 blinker", icon: "timer",
                prompt: "Blink a red LED at about 1.5 Hz with an NE555 astable timer from 5 V; decouple VCC and the CONT pin."),
        Example(title: "Sensor bridge", icon: "scalemass",
                prompt: "A 5 V excited Wheatstone bridge for a 1 kΩ strain gauge, with VA and VB outputs for an instrumentation amplifier."),
    ]

    var body: some View {
        HSplitView {
            briefPanel
                .frame(minWidth: 380, idealWidth: 480)
            agentPanel
                .frame(minWidth: 380)
        }
        .background(
            LinearGradient(colors: [Theme.navy, Theme.deepBlue.opacity(0.7)], startPoint: .top, endPoint: .bottom)
        )
    }

    // MARK: - Brief

    private var briefPanel: some View {
        VStack(alignment: .leading, spacing: 14) {
            HStack(alignment: .firstTextBaseline) {
                VStack(alignment: .leading, spacing: 4) {
                    Text("Describe your product")
                        .font(.title2.weight(.semibold))
                        .foregroundStyle(Theme.textPrimary)
                    Text("Write a prompt or paste a PRD. The agent team turns it into a verified schematic, a routed PCB and a 3D model.")
                        .font(.callout)
                        .foregroundStyle(Theme.textMuted)
                }
                Spacer()
            }

            HStack(spacing: 8) {
                Badge(text: settings.provider.shortName + " · " + settings.model(for: settings.provider),
                      systemImage: settings.provider.systemImage)
                if !settings.hasCredentials(for: settings.provider) {
                    SettingsLink {
                        Label("Add API key", systemImage: "key.fill")
                    }
                    .foregroundStyle(Theme.warning)
                    .font(.caption)
                }
                Spacer()
                Button {
                    importPRD()
                } label: { Label("Import PRD…", systemImage: "doc.badge.plus") }
                    .buttonStyle(.borderless)
                    .foregroundStyle(Theme.lightBlue)
            }

            TextEditor(text: $brief)
                .font(.system(.body, design: .default))
                .foregroundStyle(Theme.textPrimary)
                .scrollContentBackground(.hidden)
                .padding(10)
                .background(RoundedRectangle(cornerRadius: 10).fill(Theme.navy))
                .overlay(RoundedRectangle(cornerRadius: 10).strokeBorder(Theme.blue.opacity(0.45)))
                .overlay(alignment: .topLeading) {
                    if brief.isEmpty {
                        Text("e.g. “A battery-powered 9 V night light that turns on a white LED…”")
                            .foregroundStyle(Theme.textMuted)
                            .padding(16)
                            .allowsHitTesting(false)
                    }
                }
                .frame(minHeight: 180)

            Text("TEMPLATES").font(.caption.weight(.bold)).foregroundStyle(Theme.skyBlue)
            LazyVGrid(columns: [GridItem(.adaptive(minimum: 150), spacing: 8)], spacing: 8) {
                ForEach(Self.examples) { example in
                    Button {
                        brief = example.prompt
                    } label: {
                        HStack {
                            Image(systemName: example.icon).foregroundStyle(Theme.skyBlue)
                            Text(example.title).foregroundStyle(Theme.textPrimary)
                            Spacer(minLength: 0)
                        }
                        .font(.callout)
                        .padding(.horizontal, 10)
                        .padding(.vertical, 8)
                        .background(RoundedRectangle(cornerRadius: 8).fill(Theme.blue.opacity(0.14)))
                        .overlay(RoundedRectangle(cornerRadius: 8).strokeBorder(Theme.blue.opacity(0.3)))
                    }
                    .buttonStyle(.plain)
                    .help(example.prompt)
                }
            }

            HStack {
                Toggle("Review agent", isOn: $settings.enableReviewAgent)
                    .toggleStyle(.switch)
                    .controlSize(.small)
                    .foregroundStyle(Theme.textSecondary)
                Spacer()
                if agents.isRunning {
                    Button(role: .cancel) { agents.cancel() } label: { Label("Stop", systemImage: "stop.fill") }
                        .controlSize(.large)
                }
                Button {
                    agents.generate(brief: brief, store: store, settings: settings)
                } label: {
                    Label("Generate Design", systemImage: "sparkles")
                        .padding(.horizontal, 8)
                }
                .buttonStyle(.borderedProminent)
                .controlSize(.large)
                .keyboardShortcut(.return, modifiers: .command)
                .disabled(agents.isRunning || brief.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty)
            }
        }
        .padding(20)
    }

    private func importPRD() {
        let panel = NSOpenPanel()
        panel.allowedContentTypes = [.plainText, .text, UTType(filenameExtension: "md") ?? .plainText]
        guard panel.runModal() == .OK, let url = panel.url,
              let text = try? String(contentsOf: url, encoding: .utf8) else { return }
        brief = text
    }

    // MARK: - Agents

    private var agentPanel: some View {
        VStack(alignment: .leading, spacing: 12) {
            HStack {
                Label("Agent Pipeline", systemImage: "person.3.sequence")
                    .font(.headline)
                    .foregroundStyle(Theme.textPrimary)
                Spacer()
                if !agents.activeModel.isEmpty {
                    Text(agents.activeModel).font(.caption).foregroundStyle(Theme.textMuted)
                }
                Button {
                    agents.clearConversation()
                } label: { Image(systemName: "trash") }
                    .buttonStyle(.borderless)
                    .foregroundStyle(Theme.lightBlue)
                    .disabled(agents.isRunning)
                    .help("Clear conversation")
            }

            if agents.steps.isEmpty {
                pipelinePreview
            } else {
                VStack(spacing: 6) {
                    ForEach(agents.steps) { step in StepRow(step: step) }
                }
            }

            Divider().overlay(Theme.blue.opacity(0.3))

            ScrollViewReader { proxy in
                ScrollView {
                    LazyVStack(alignment: .leading, spacing: 10) {
                        ForEach(agents.messages) { message in
                            MessageBubble(message: message).id(message.id)
                        }
                    }
                    .padding(.vertical, 4)
                }
                .onChange(of: agents.messages.count) { _, _ in
                    if let last = agents.messages.last { withAnimation { proxy.scrollTo(last.id, anchor: .bottom) } }
                }
            }

            HStack {
                TextField("Ask for a change — “make the LED green and run it from 3.3 V”", text: $refinement)
                    .textFieldStyle(.roundedBorder)
                    .onSubmit(sendRefinement)
                    .disabled(store.snapshot.components.isEmpty)
                Button(action: sendRefinement) { Image(systemName: "arrow.up.circle.fill").font(.title2) }
                    .buttonStyle(.borderless)
                    .foregroundStyle(Theme.blue)
                    .disabled(refinement.isEmpty || agents.isRunning || store.snapshot.components.isEmpty)
            }
        }
        .padding(16)
        .bluePanel()
        .padding(12)
    }

    private func sendRefinement() {
        guard !refinement.isEmpty, !agents.isRunning else { return }
        agents.refine(instruction: refinement, store: store, settings: settings)
        refinement = ""
    }

    private var pipelinePreview: some View {
        VStack(alignment: .leading, spacing: 8) {
            ForEach([AgentOrchestrator.Role.analyst, .architect, .compiler, .verifier, .reviewer, .layout], id: \.self) { role in
                HStack(spacing: 10) {
                    Image(systemName: role.systemImage).foregroundStyle(Theme.blue).frame(width: 22)
                    Text(role.rawValue).foregroundStyle(Theme.textSecondary)
                    Spacer()
                    Image(systemName: "circle.dotted").foregroundStyle(Theme.textMuted)
                }
                .font(.callout)
            }
        }
        .padding(12)
        .background(RoundedRectangle(cornerRadius: 10).fill(Theme.navy.opacity(0.6)))
    }
}

private struct StepRow: View {
    var step: AgentOrchestrator.Step

    var body: some View {
        HStack(alignment: .top, spacing: 10) {
            Image(systemName: step.role.systemImage)
                .foregroundStyle(Theme.blue)
                .frame(width: 22)
            VStack(alignment: .leading, spacing: 2) {
                HStack {
                    Text(step.role.rawValue).font(.callout.weight(.semibold)).foregroundStyle(Theme.textPrimary)
                    Text(step.title).font(.callout).foregroundStyle(Theme.textMuted)
                }
                if !step.detail.isEmpty {
                    Text(step.detail).font(.caption).foregroundStyle(Theme.textSecondary).textSelection(.enabled)
                }
            }
            Spacer()
            if let duration = step.duration {
                Text(String(format: "%.1fs", duration)).font(.caption.monospacedDigit()).foregroundStyle(Theme.textMuted)
            }
            statusIcon
        }
        .padding(8)
        .background(RoundedRectangle(cornerRadius: 8).fill(Theme.navy.opacity(0.55)))
    }

    @ViewBuilder
    private var statusIcon: some View {
        switch step.status {
        case .running: ProgressView().controlSize(.small)
        case .done: Image(systemName: "checkmark.circle.fill").foregroundStyle(Theme.skyBlue)
        case .warning: Image(systemName: "exclamationmark.triangle.fill").foregroundStyle(Theme.warning)
        case .failed: Image(systemName: "xmark.octagon.fill").foregroundStyle(Theme.error)
        case .skipped: Image(systemName: "minus.circle").foregroundStyle(Theme.textMuted)
        }
    }
}

private struct MessageBubble: View {
    var message: AgentOrchestrator.Message

    var body: some View {
        HStack {
            if message.sender == .user { Spacer(minLength: 40) }
            VStack(alignment: .leading, spacing: 4) {
                Text(senderTitle).font(.caption.weight(.semibold)).foregroundStyle(Theme.skyBlue)
                Text(LocalizedStringKey(message.text))
                    .foregroundStyle(Theme.textPrimary)
                    .textSelection(.enabled)
            }
            .padding(10)
            .background(
                RoundedRectangle(cornerRadius: 10)
                    .fill(message.sender == .user ? Theme.blue.opacity(0.35) : Theme.navy.opacity(0.7))
            )
            .overlay(RoundedRectangle(cornerRadius: 10).strokeBorder(Theme.blue.opacity(0.25)))
            if message.sender != .user { Spacer(minLength: 40) }
        }
    }

    private var senderTitle: String {
        switch message.sender {
        case .user: return "You"
        case .agent: return "SiEDA Agents"
        case .system: return "System"
        }
    }
}
