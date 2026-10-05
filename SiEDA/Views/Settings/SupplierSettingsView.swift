import SwiftUI

/// Settings → Suppliers: API keys for Octopart (Nexar), DigiKey and Mouser (kept in the macOS Keychain), the
/// currency prices are shown in and how long search results are cached.
struct SupplierSettingsView: View {
    @ObservedObject private var settings = SupplierSettings.shared
    @State private var drafts: [SupplierCredentialField: String] = [:]
    @State private var cleared = false

    var body: some View {
        Form {
            Section {
                Text("SiEDA searches distributors directly with your own API keys: nothing goes through a SiEDA server. Without a key a distributor is simply not searched.")
                    .font(.caption).foregroundStyle(Theme.textMuted)
                Picker("Currency", selection: $settings.currency) {
                    ForEach(SupplierSettings.currencies, id: \.self) { Text(verbatim: $0).tag($0) }
                }
                Stepper(value: $settings.cacheHours, in: 1...720) {
                    Text("Keep results for \(settings.cacheHours) hours")
                }
                HStack {
                    Button("Clear Cached Results") {
                        settings.cache.clear()
                        cleared = true
                    }
                    if cleared { Text("Cleared").font(.caption).foregroundStyle(Theme.textMuted) }
                }
            } header: {
                Text("Search")
            }
            ForEach(SupplierSource.allCases) { source in
                Section {
                    ForEach(SupplierCredentialField.fields(for: source), id: \.self) { field in
                        credentialRow(field)
                    }
                    HStack {
                        if settings.credentials.isConfigured(source) {
                            Label("Configured", systemImage: "checkmark.circle.fill").foregroundStyle(Theme.success)
                        } else {
                            Label("Not configured", systemImage: "circle.dashed").foregroundStyle(Theme.textMuted)
                        }
                        Spacer()
                        if let url = source.signUpURL { Link("Get an API key", destination: url) }
                    }
                    .font(.caption)
                } header: {
                    Text(verbatim: source.displayName)
                } footer: {
                    Text(Self.footer(source)).font(.caption2).foregroundStyle(Theme.textMuted)
                }
            }
        }
        .formStyle(.grouped)
        .id(settings.revision)
    }

    private func credentialRow(_ field: SupplierCredentialField) -> some View {
        let binding = Binding(get: { drafts[field] ?? settings.value(field) }, set: { drafts[field] = $0 })
        return HStack {
            Group {
                if field.isSecret {
                    SecureField(Self.label(field), text: binding)
                } else {
                    TextField(Self.label(field), text: binding)
                }
            }
            .onSubmit { save(field) }
            Button("Save") { save(field) }
                .disabled((drafts[field] ?? settings.value(field)) == settings.value(field))
            Button("Remove") {
                drafts[field] = ""
                settings.setValue("", for: field)
            }
            .disabled(settings.value(field).isEmpty)
        }
    }

    private func save(_ field: SupplierCredentialField) {
        guard let draft = drafts[field] else { return }
        settings.setValue(draft, for: field)
        drafts[field] = nil
    }

    static func label(_ field: SupplierCredentialField) -> LocalizedStringKey {
        switch field {
        case .nexarClientId, .digikeyClientId: return "Client ID"
        case .nexarClientSecret, .digikeyClientSecret: return "Client secret"
        case .mouserApiKey: return "Search API key"
        }
    }

    /// Which kind of key each distributor issues.
    static func footer(_ source: SupplierSource) -> LocalizedStringKey {
        switch source {
        case .nexar:
            return "Nexar application with the Supply scope (client credentials). Octopart data: offers from many distributors in one search."
        case .digikey:
            return "DigiKey API application (Production) with Product Information v4; client credentials flow."
        case .mouser:
            return "Mouser Search API key (API Hub → Search API)."
        }
    }
}
