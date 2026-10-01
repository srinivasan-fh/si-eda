#!/usr/bin/env bash
# SiEDA local runner — build, test and launch the project from a terminal.
#
#   ./run.sh                 macOS: build and launch the app · Linux: build the core and run the demo
#   ./run.sh app             build the macOS app (Debug) and launch it
#   ./run.sh xcode           open the project in Xcode (then press ⌘R)
#   ./run.sh test            core unit tests, plus the Xcode tests on macOS
#   ./run.sh core            build the C++ core, CLI and unit tests (any platform)
#   ./run.sh demo [dir]      run the headless pipeline on the demo design, fabrication files into dir (default: out/)
#   ./run.sh cli <file.siedaproj> [dir]   verify a saved project and write its fabrication files
#   ./run.sh clean           remove local build folders
#   ./run.sh doctor          check the tools this script needs
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$ROOT"

CORE_BUILD="$ROOT/build"
APP_BUILD="$ROOT/build-app"   # Xcode DerivedData for command-line app builds
PROJECT="SiEDA.xcodeproj"
SCHEME="SiEDA"

if [ -t 1 ]; then
    BLUE=$'\033[1;34m'; GREEN=$'\033[1;32m'; RED=$'\033[1;31m'; YELLOW=$'\033[1;33m'; RESET=$'\033[0m'
else
    BLUE=""; GREEN=""; RED=""; YELLOW=""; RESET=""
fi
step() { printf '%s==>%s %s\n' "$BLUE" "$RESET" "$*"; }
ok()   { printf '%s✓%s %s\n' "$GREEN" "$RESET" "$*"; }
warn() { printf '%s!%s %s\n' "$YELLOW" "$RESET" "$*" >&2; }
die()  { printf '%s✗%s %s\n' "$RED" "$RESET" "$*" >&2; exit 1; }

is_macos() { [ "$(uname -s)" = "Darwin" ]; }
have() { command -v "$1" >/dev/null 2>&1; }

jobs_count() {
    if have sysctl && is_macos; then sysctl -n hw.ncpu
    elif have nproc; then nproc
    else echo 4; fi
}

require_cmake() {
    have cmake || die "CMake 3.20+ is required (macOS: 'brew install cmake' · Ubuntu: 'sudo apt install cmake g++')."
}

require_xcode() {
    is_macos || die "The SiEDA app is a macOS app (macOS 14+, Xcode 16+). On Linux use: ./run.sh core | test | demo"
    have xcodebuild || die "Xcode is not installed. Install Xcode 16+ from the App Store, then run: sudo xcode-select -s /Applications/Xcode.app"
    if ! xcodebuild -version >/dev/null 2>&1; then
        die "xcodebuild needs the full Xcode, not only the Command Line Tools. Run: sudo xcode-select -s /Applications/Xcode.app"
    fi
    local major
    major="$(sw_vers -productVersion | cut -d. -f1)"
    [ "$major" -ge 14 ] || die "macOS 14 Sonoma or later is required (this Mac runs $(sw_vers -productVersion))."
    local xcode
    xcode="$(xcodebuild -version | awk 'NR==1 {print $2}' | cut -d. -f1)"
    [ "${xcode:-0}" -ge 16 ] || warn "Xcode 16 or later is recommended (found $(xcodebuild -version | head -1))."
}

# Keep the Xcode project in sync with the source tree (CI fails if it is out of date).
sync_xcode_project() {
    if have python3; then
        python3 tools/generate_xcodeproj.py >/dev/null
    else
        warn "python3 not found: skipping Xcode project regeneration."
    fi
}

build_core() {
    require_cmake
    step "Configuring the C++ core (Release)"
    cmake -S . -B "$CORE_BUILD" -DCMAKE_BUILD_TYPE=Release >/dev/null
    step "Building the core, CLI and unit tests"
    cmake --build "$CORE_BUILD" --parallel "$(jobs_count)"
    ok "Core built: $CORE_BUILD/sieda-cli, $CORE_BUILD/sieda_core_tests"
}

test_core() {
    build_core
    step "Running the core unit tests"
    ctest --test-dir "$CORE_BUILD" --output-on-failure
    ok "Core tests passed"
}

run_demo() {
    local dir="${1:-out}"
    build_core
    mkdir -p "$dir"
    step "Running the headless pipeline (ERC → simulation → place & route → DRC → verification → fabrication files)"
    "$CORE_BUILD/sieda-cli" --demo "$dir"
    ok "Fabrication files and verification_report.md written to $dir/"
}

run_cli() {
    [ $# -ge 1 ] || die "Usage: ./run.sh cli <design.siedaproj> [output-dir]"
    [ -f "$1" ] || die "No such project file: $1"
    build_core
    [ $# -ge 2 ] && mkdir -p "$2"
    step "Processing $1"
    "$CORE_BUILD/sieda-cli" "$@"
}

build_app() {
    require_xcode
    sync_xcode_project
    step "Building SiEDA.app (Debug) — the first build takes a few minutes"
    local log="$APP_BUILD/xcodebuild.log"
    mkdir -p "$APP_BUILD"
    if ! xcodebuild -project "$PROJECT" -scheme "$SCHEME" -configuration Debug \
            -destination 'platform=macOS' -derivedDataPath "$APP_BUILD" \
            CODE_SIGN_IDENTITY=- build >"$log" 2>&1; then
        grep -E "(error|fatal error): " "$log" | head -20 >&2 || true
        die "App build failed — full log: $log"
    fi
    APP_PATH="$APP_BUILD/Build/Products/Debug/SiEDA.app"
    [ -d "$APP_PATH" ] || die "Build finished but $APP_PATH was not found (log: $log)"
    ok "Built $APP_PATH"
}

launch_app() {
    build_app
    step "Launching SiEDA"
    open "$APP_PATH"
    ok "SiEDA is running. Add an API key in Settings → AI Models, or use the Offline Designer (no key needed)."
}

test_app() {
    require_xcode
    sync_xcode_project
    step "Running the Xcode tests"
    mkdir -p "$APP_BUILD"
    local log="$APP_BUILD/xcodebuild-test.log"
    if ! xcodebuild -project "$PROJECT" -scheme "$SCHEME" -configuration Debug \
            -destination 'platform=macOS' -derivedDataPath "$APP_BUILD" \
            CODE_SIGN_IDENTITY=- test >"$log" 2>&1; then
        grep -E "error: |Test Case .* failed|\*\* TEST FAILED" "$log" | head -30 >&2 || true
        die "Xcode tests failed — full log: $log"
    fi
    ok "Xcode tests passed"
}

open_xcode() {
    is_macos || die "Xcode is only available on macOS."
    sync_xcode_project
    step "Opening $PROJECT — press ⌘R to build and run"
    open "$PROJECT"
}

doctor() {
    printf 'System:   %s %s\n' "$(uname -s)" "$(uname -m)"
    if is_macos; then printf 'macOS:    %s (14+ required for the app)\n' "$(sw_vers -productVersion)"; fi
    for tool in cmake python3 xcodebuild git; do
        if have "$tool"; then
            case "$tool" in
                xcodebuild) printf '%-9s %s\n' "$tool:" "$(xcodebuild -version 2>/dev/null | head -1 || echo 'Command Line Tools only — install Xcode')";;
                *) printf '%-9s %s\n' "$tool:" "$("$tool" --version 2>&1 | head -1)";;
            esac
        else
            printf '%-9s %s\n' "$tool:" "not found"
        fi
    done
}

usage() { sed -n '2,/^set -euo/p' "$0" | sed '$d' | sed 's/^# \{0,1\}//'; }

command="${1:-}"
[ $# -gt 0 ] && shift
case "$command" in
    "")       if is_macos; then launch_app; else run_demo out; fi ;;
    app|run)  launch_app ;;
    build)    if is_macos; then build_app; fi; build_core ;;
    xcode)    open_xcode ;;
    test)     test_core; if is_macos && have xcodebuild && xcodebuild -version >/dev/null 2>&1; then test_app; else warn "Skipping the Xcode tests (macOS with Xcode only)."; fi ;;
    core)     build_core ;;
    demo)     run_demo "$@" ;;
    cli)      run_cli "$@" ;;
    clean)    rm -rf "$CORE_BUILD" "$APP_BUILD" out; ok "Removed build/, build-app/ and out/" ;;
    doctor)   doctor ;;
    -h|--help|help) usage ;;
    *)        usage; die "Unknown command: $command" ;;
esac
