import AppKit
import SwiftUI

/// Interface languages: English plus the languages of the world's semiconductor countries and of India's
/// semiconductor states. Translations live in `SiEDA/Resources/<code>.lproj/Localizable.strings`.
struct AppLanguage: Identifiable, Hashable {
    enum Group: String, CaseIterable {
        case global = "Global semiconductor countries"
        case india = "Indian semiconductor states"
    }

    /// The `.lproj` / `AppleLanguages` code (BCP 47).
    let code: String
    /// The language's name in its own script, shown in the picker.
    let nativeName: String
    /// The English name.
    let englishName: String
    /// Where it is spoken, among the regions SiEDA targets.
    let regions: String
    let group: Group

    var id: String { code }

    static let english = AppLanguage(code: "en", nativeName: "English", englishName: "English",
                                     regions: "United States, Singapore, India, Philippines, Malaysia", group: .global)

    /// Every interface language. English is the development language.
    static let all: [AppLanguage] = [
        english,
        AppLanguage(code: "zh-Hant", nativeName: "繁體中文", englishName: "Chinese, Traditional",
                    regions: "Taiwan", group: .global),
        AppLanguage(code: "ko", nativeName: "한국어", englishName: "Korean", regions: "South Korea", group: .global),
        AppLanguage(code: "zh-Hans", nativeName: "简体中文", englishName: "Chinese, Simplified",
                    regions: "China, Singapore, Malaysia", group: .global),
        AppLanguage(code: "ja", nativeName: "日本語", englishName: "Japanese", regions: "Japan", group: .global),
        AppLanguage(code: "ms", nativeName: "Bahasa Melayu", englishName: "Malay",
                    regions: "Malaysia, Singapore", group: .global),
        AppLanguage(code: "nl", nativeName: "Nederlands", englishName: "Dutch", regions: "Netherlands", group: .global),
        AppLanguage(code: "de", nativeName: "Deutsch", englishName: "German", regions: "Germany", group: .global),
        AppLanguage(code: "vi", nativeName: "Tiếng Việt", englishName: "Vietnamese", regions: "Vietnam", group: .global),
        AppLanguage(code: "fil", nativeName: "Filipino", englishName: "Filipino", regions: "Philippines", group: .global),
        AppLanguage(code: "hi", nativeName: "हिन्दी", englishName: "Hindi",
                    regions: "India · Uttar Pradesh, Rajasthan", group: .india),
        AppLanguage(code: "te", nativeName: "తెలుగు", englishName: "Telugu",
                    regions: "Andhra Pradesh, Telangana", group: .india),
        AppLanguage(code: "as", nativeName: "অসমীয়া", englishName: "Assamese", regions: "Assam", group: .india),
        AppLanguage(code: "gu", nativeName: "ગુજરાતી", englishName: "Gujarati", regions: "Gujarat", group: .india),
        AppLanguage(code: "kn", nativeName: "ಕನ್ನಡ", englishName: "Kannada", regions: "Karnataka", group: .india),
        AppLanguage(code: "ml", nativeName: "മലയാളം", englishName: "Malayalam", regions: "Kerala", group: .india),
        AppLanguage(code: "mr", nativeName: "मराठी", englishName: "Marathi", regions: "Maharashtra", group: .india),
        AppLanguage(code: "or", nativeName: "ଓଡ଼ିଆ", englishName: "Odia", regions: "Odisha", group: .india),
        AppLanguage(code: "pa", nativeName: "ਪੰਜਾਬੀ", englishName: "Punjabi", regions: "Punjab", group: .india),
        AppLanguage(code: "ta", nativeName: "தமிழ்", englishName: "Tamil",
                    regions: "Tamil Nadu, Singapore", group: .india),
    ]

    static func find(_ code: String) -> AppLanguage? { all.first { $0.code == code } }
}

/// The interface language choice. "" follows the system; otherwise one of `AppLanguage.all`.
///
/// SwiftUI views switch at once through the `locale` environment. The menu bar and AppKit panels read
/// `AppleLanguages` at launch, so they follow after a relaunch.
@MainActor
final class LanguageSettings: ObservableObject {
    static let defaultsKey = "interfaceLanguage"

    private let defaults: UserDefaults

    @Published var code: String {
        didSet {
            guard code != oldValue else { return }
            Self.store(code, in: defaults)
            needsRelaunch = code != launchCode
        }
    }
    /// True when the menus still show the language the app was launched in.
    @Published private(set) var needsRelaunch = false
    private let launchCode: String

    init(defaults: UserDefaults = .standard) {
        self.defaults = defaults
        let saved = defaults.string(forKey: Self.defaultsKey) ?? ""
        let valid = saved.isEmpty || AppLanguage.find(saved) != nil ? saved : ""
        code = valid
        launchCode = valid
    }

    var selected: AppLanguage? { AppLanguage.find(code) }

    /// The locale SwiftUI views render with: the chosen language, or the system's.
    var locale: Locale { code.isEmpty ? .autoupdatingCurrent : Locale(identifier: code) }

    /// Writes the choice and the matching `AppleLanguages` override for the next launch.
    static func store(_ code: String, in defaults: UserDefaults) {
        if code.isEmpty || AppLanguage.find(code) == nil {
            defaults.removeObject(forKey: defaultsKey)
            defaults.removeObject(forKey: "AppleLanguages")
        } else {
            defaults.set(code, forKey: defaultsKey)
            defaults.set([code, "en"], forKey: "AppleLanguages")
        }
    }

    /// Looks a UI string up in the chosen language (for text drawn outside SwiftUI's `Text`).
    static func localized(_ key: String, code: String) -> String {
        let fallback = Bundle.main.localizedString(forKey: key, value: key, table: nil)
        guard !code.isEmpty, let path = Bundle.main.path(forResource: code, ofType: "lproj"),
              let bundle = Bundle(path: path) else { return fallback }
        return bundle.localizedString(forKey: key, value: fallback, table: nil)
    }

    /// Set by `relaunch()`; the app delegate starts SiEDA again once this instance has really quit.
    static var relaunchRequested = false

    /// Quits through the normal path (so unsaved changes are still asked about) and opens SiEDA again, so the menus
    /// switch language too. If the quit is cancelled, nothing relaunches.
    func relaunch() {
        Self.relaunchRequested = true
        NSApp.terminate(nil)
        // Still running: the quit was cancelled at the (modal) unsaved-changes prompt.
        Self.relaunchRequested = false
    }

    /// Opens the app bundle again after this process exits (called from `applicationWillTerminate`).
    static func launchAgainAfterExit() {
        let process = Process()
        process.executableURL = URL(fileURLWithPath: "/bin/sh")
        process.arguments = ["-c", "while kill -0 \(ProcessInfo.processInfo.processIdentifier) 2>/dev/null; do sleep 0.2; done; "
                             + "/usr/bin/open -n \"$0\"", Bundle.main.bundlePath]
        try? process.run()
    }
}

/// Settings → Language.
struct LanguageSettingsView: View {
    @EnvironmentObject private var language: LanguageSettings

    var body: some View {
        Form {
            Section {
                Picker("Interface language", selection: $language.code) {
                    Text("System default").tag("")
                    ForEach(AppLanguage.Group.allCases, id: \.self) { group in
                        Divider()
                        ForEach(AppLanguage.all.filter { $0.group == group }) { lang in
                            Text(verbatim: lang.code == "en" ? lang.nativeName
                                 : "\(lang.nativeName) — \(lang.englishName)").tag(lang.code)
                        }
                    }
                }
                Text("The interface follows this language. Engineering terms, part numbers, units, net names and design check messages stay in English so they match datasheets and fabrication files.")
                    .font(.caption)
                    .foregroundStyle(Theme.textMuted)
                if language.needsRelaunch {
                    HStack {
                        Text("Restart SiEDA to finish switching the menus to the new language.")
                            .font(.callout)
                        Spacer()
                        Button("Relaunch Now") { language.relaunch() }
                    }
                }
            } header: {
                Text("Language")
            }

            Section {
                ForEach(AppLanguage.Group.allCases, id: \.self) { group in
                    VStack(alignment: .leading, spacing: 4) {
                        Text(LocalizedStringKey(group.rawValue)).font(.headline)
                        ForEach(AppLanguage.all.filter { $0.group == group }) { lang in
                            HStack {
                                Text(verbatim: lang.nativeName)
                                Spacer()
                                Text(verbatim: lang.regions).foregroundStyle(Theme.textMuted)
                            }
                            .font(.callout)
                        }
                    }
                }
            }
        }
        .formStyle(.grouped)
    }
}
