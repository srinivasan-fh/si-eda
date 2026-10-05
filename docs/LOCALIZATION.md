# Interface Languages

SiEDA's interface is in English and 19 other languages. Pick one in **Settings → Language → Interface language**.
**System default** follows the macOS language list.

- [Languages](#languages)
- [Switching](#switching)
- [What is translated](#what-is-translated)
- [Adding or changing a string](#adding-or-changing-a-string)
- [Adding a language](#adding-a-language)
- [Code map](#code-map)
- [Tests](#tests)

## Languages

**Global semiconductor countries**

| Country | Language | Code |
|---|---|---|
| Taiwan | 繁體中文 Chinese, Traditional | `zh-Hant` |
| South Korea | 한국어 Korean | `ko` |
| China | 简体中文 Chinese, Simplified | `zh-Hans` |
| United States | English | `en` |
| Japan | 日本語 Japanese | `ja` |
| Malaysia | Bahasa Melayu Malay (also 简体中文, English) | `ms` |
| Netherlands | Nederlands Dutch | `nl` |
| Germany | Deutsch German | `de` |
| Singapore | English, 简体中文, Bahasa Melayu, தமிழ் — its four official languages | `en` `zh-Hans` `ms` `ta` |
| Vietnam | Tiếng Việt Vietnamese | `vi` |
| Philippines | Filipino (also English) | `fil` |
| India | हिन्दी Hindi (also English) | `hi` |

**Indian semiconductor states**

| State | Language | Code |
|---|---|---|
| Andhra Pradesh | తెలుగు Telugu | `te` |
| Assam | অসমীয়া Assamese | `as` |
| Gujarat | ગુજરાતી Gujarati | `gu` |
| Karnataka | ಕನ್ನಡ Kannada | `kn` |
| Kerala | മലയാളം Malayalam | `ml` |
| Maharashtra | मराठी Marathi | `mr` |
| Odisha | ଓଡ଼ିଆ Odia | `or` |
| Punjab | ਪੰਜਾਬੀ Punjabi (Gurmukhi) | `pa` |
| Rajasthan | हिन्दी Hindi | `hi` |
| Tamil Nadu | தமிழ் Tamil | `ta` |
| Telangana | తెలుగు Telugu | `te` |
| Uttar Pradesh | हिन्दी Hindi | `hi` |

## Switching

- **Views** (workspaces, toolbars, panels, inspectors, editors, settings) switch at once.
- **The menu bar, the splash screen and AppKit panels** (open / save) read the language at launch. After a change,
  Settings shows **Relaunch Now**. Use it, or quit and reopen SiEDA.
- The choice is saved in the app's defaults (`interfaceLanguage`, plus `AppleLanguages = [code, en]` for the next
  launch). **System default** removes both.

## What is translated

Translated: the interface itself, which is the menus, toolbar, sidebar, workspace names, buttons, pickers, section
headers, editor tools, settings, help tags and the splash progress steps.

Kept in English on purpose, so a design reads the same as its datasheets, fabrication files and the team's other
tools:

- part numbers, reference designators, net names and pin names;
- units and engineering values;
- standards (IPC-2221, IEC 60601, …);
- ERC / DRC / verification messages and their codes;
- AI agent output, which follows the language you write your prompt in;
- exported files (Gerber, drill, BOM, netlists, reports).

Strings built at run time with values in them (for example "12 parts") stay in English until they are given a
format key.

**Review before relying on a language.** The translations were made for this release and checked by tooling (every
key present, format specifiers kept). They have not yet been reviewed by native-speaking engineers for every
language. Send corrections as edits to the `.strings` file. They are plain text.

## Adding or changing a string

1. Write the English text in the view: `Text("…")`, `Button("…")`, `Label("…", systemImage:)`, `.help("…")`,
   `Toggle`, `Picker`, `Section`, … SwiftUI treats a string literal as a localization key. For text that comes from
   a `String` value, use `Text(LocalizedStringKey(value))`, or `LanguageSettings.localized(_:code:)` outside SwiftUI.
2. Add the key to **every** `SiEDA/Resources/<code>.lproj/Localizable.strings`. In `en`, the value is the key.
3. Run `python3 tools/check_localization.py --missing`. It fails on a missing or extra key, an empty value or a
   changed format specifier, and it lists the UI strings in the sources that are not translated yet. CI runs it
   (without `--missing`) and `plutil -lint` on every file.

## Adding a language

1. Add an `AppLanguage` entry in `SiEDA/App/AppLanguage.swift`.
2. Add `SiEDA/Resources/<code>.lproj/Localizable.strings` with every English key.
3. Add the code to `LANGUAGES` in `tools/check_localization.py`.
4. Run `python3 tools/generate_xcodeproj.py`. It adds the language to the project's variant group and known
   regions.

## Code map

| What | Where |
|---|---|
| Languages, the choice, relaunch, the Settings tab | `SiEDA/App/AppLanguage.swift` (`AppLanguage`, `LanguageSettings`, `LanguageSettingsView`) |
| Locale applied to every window | `SiEDA/App/SiEDAApp.swift` (`.environment(\.locale, …)`) |
| Translations | `SiEDA/Resources/<code>.lproj/Localizable.strings` |
| Project wiring | `tools/generate_xcodeproj.py` (variant group `Localizable.strings`, `knownRegions`) |
| Checker | `tools/check_localization.py` (CI: *Interface translations are complete*) |

## Tests

`LocalizationTests` (app):

- every language ships a complete table that parses, and most of each table is translated;
- the languages cover the target countries and states;
- lookups use the chosen language and fall back to English;
- the choice persists and sets `AppleLanguages`, an unknown code falls back to the system language, and relaunch
  is offered only after a change;
- the Settings tab renders.
