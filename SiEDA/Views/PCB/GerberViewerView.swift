// SiEDA — Gerber viewer: Gerber (RS-274X / X2) and Excellon drill files shown as layers over each other, read by the
// core (sieda_gerber_parse). Any file can stay on the board as a reference layer.
import AppKit
import SwiftUI
import UniformTypeIdentifiers

struct GerberViewerView: View {
    @EnvironmentObject private var store: DesignStore
    @Environment(\.dismiss) private var dismiss

    struct Layer: Identifiable {
        let id = UUID()
        var name: String
        var text: String
        var art: GerberArtwork
        var visible = true
    }

    @State private var layers: [Layer] = []
    @State private var message = ""

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            HStack {
                Text("Gerber Viewer").font(.headline)
                Spacer()
                Button("Open Files…", action: open)
                Button("Done") { dismiss() }.keyboardShortcut(.defaultAction)
            }
            HStack(alignment: .top, spacing: 12) {
                List {
                    ForEach(Array(layers.enumerated()), id: \.element.id) { index, layer in
                        HStack(spacing: 6) {
                            Toggle("", isOn: $layers[index].visible).labelsHidden()
                            Circle().fill(Theme.referenceColour(index)).frame(width: 10, height: 10)
                            VStack(alignment: .leading, spacing: 1) {
                                Text(verbatim: layer.name).font(.caption)
                                Text(verbatim: "\(layer.art.kind) · \(layer.art.shapeCount)").font(.caption2).foregroundStyle(Theme.textMuted)
                            }
                            Spacer()
                            Button("Add to Board") { store.addReferenceLayer(name: layer.name, text: layer.text) }
                                .controlSize(.small)
                                .help("Keep this file over the board as a reference layer")
                        }
                    }
                }
                .frame(width: 300)
                artwork
            }
            Text(verbatim: message).font(.caption).foregroundStyle(Theme.textMuted)
        }
        .padding(16)
        .frame(minWidth: 900, minHeight: 600)
    }

    private var artwork: some View {
        Canvas { ctx, size in
            ctx.fill(Path(CGRect(origin: .zero, size: size)), with: .color(Theme.pcbBackground))
            let shown = layers.enumerated().filter { $0.element.visible && !$0.element.art.bounds.isNull }
            guard let first = shown.first?.element.art.bounds else { return }
            let all = shown.reduce(first) { $0.union($1.element.art.bounds) }
            let pad = 16.0
            let scale = min((size.width - 2 * pad) / max(all.width, 0.1), (size.height - 2 * pad) / max(all.height, 0.1))
            // File coordinates (Y up) → view (Y down), fitted.
            let fit = CGAffineTransform(a: scale, b: 0, c: 0, d: -scale,
                                        tx: pad - all.minX * scale, ty: size.height - pad + all.minY * scale)
            for (index, layer) in shown {
                let colour = Theme.referenceColour(index)
                ctx.fill(Path(layer.art.fills).applying(fit), with: .color(colour.opacity(0.7)))
                for (width, path) in layer.art.strokes {
                    ctx.stroke(Path(path).applying(fit), with: .color(colour.opacity(0.85)),
                               style: StrokeStyle(lineWidth: max(0.5, width * scale), lineCap: .round))
                }
                ctx.fill(Path(layer.art.clearFills).applying(fit), with: .color(Theme.pcbBackground))
                for (width, path) in layer.art.clearStrokes {
                    ctx.stroke(Path(path).applying(fit), with: .color(Theme.pcbBackground),
                               style: StrokeStyle(lineWidth: max(0.5, width * scale), lineCap: .round))
                }
            }
        }
        .accessibilityLabel(Text("Gerber Viewer"))
    }

    private func open() {
        let panel = NSOpenPanel()
        panel.allowsMultipleSelection = true
        panel.allowedContentTypes = [.data]
        panel.message = String(localized: "Open Gerber and drill files to view them.")
        guard panel.runModal() == .OK else { return }
        var skipped: [String] = []
        for url in panel.urls {
            guard let text = (try? String(contentsOf: url, encoding: .utf8)) ?? (try? String(contentsOf: url, encoding: .isoLatin1)),
                  let art = EDAEngine.gerberArtwork(text), art.error.isEmpty else {
                skipped.append(url.lastPathComponent)
                continue
            }
            layers.append(Layer(name: url.lastPathComponent, text: text, art: art))
        }
        message = skipped.isEmpty ? "" : String(format: String(localized: "Not Gerber or drill files: %@"), skipped.joined(separator: ", "))
    }
}
