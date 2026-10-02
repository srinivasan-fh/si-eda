#!/usr/bin/env python3
"""Generates SiEDA.xcodeproj (macOS SwiftUI app + C++ core + unit tests) deterministically.

Run from anywhere:  python3 tools/generate_xcodeproj.py
Re-run after adding or removing source files. Object IDs are stable hashes of their paths,
so regenerating produces minimal diffs.
"""
import hashlib
import os
import re

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PROJECT = os.path.join(ROOT, "SiEDA.xcodeproj")

APP = "SiEDA"
TESTS = "SiEDATests"
BUNDLE_ID = "com.sieda.SiEDA"
DEPLOYMENT = "14.0"


def q(value):
    """Quotes an OpenStep plist string when it contains characters outside the unquoted set."""
    return value if re.fullmatch(r"[A-Za-z0-9_./]+", value) else '"' + value.replace('"', '\\"') + '"'


def oid(*parts):
    return hashlib.md5("/".join(parts).encode()).hexdigest()[:24].upper()


def collect(folder, exts):
    out = []
    base = os.path.join(ROOT, folder)
    for dirpath, dirnames, filenames in os.walk(base):
        dirnames[:] = sorted(d for d in dirnames if not d.endswith(".xcassets"))
        for f in sorted(filenames):
            if os.path.splitext(f)[1] in exts:
                out.append(os.path.relpath(os.path.join(dirpath, f), ROOT))
    return sorted(out)


FILE_TYPES = {
    ".swift": "sourcecode.swift",
    ".cpp": "sourcecode.cpp.cpp",
    ".hpp": "sourcecode.cpp.h",
    ".h": "sourcecode.c.h",
    ".xcassets": "folder.assetcatalog",
    ".entitlements": "text.plist.entitlements",
    ".md": "net.daringfireball.markdown",
    ".plist": "text.plist.xml",
}

app_swift = collect("SiEDA", {".swift"})
core_cpp = collect("Core/src", {".cpp"})
core_headers = collect("Core/include", {".hpp", ".h"})
bridge_headers = collect("SiEDA", {".h"})
test_swift = collect("SiEDATests", {".swift"})
assets = "SiEDA/Assets.xcassets"
entitlements = "SiEDA/SiEDA.entitlements"
info_plist = "SiEDA/Info.plist"  # document types and UTIs, merged into the generated Info.plist
docs = [p for p in ["README.md", "docs/ARCHITECTURE.md", "docs/PRD.md"] if os.path.exists(os.path.join(ROOT, p))]

all_files = app_swift + core_cpp + core_headers + bridge_headers + test_swift + [assets, entitlements, info_plist] + docs

# ---------------------------------------------------------------- objects
objects = {}  # id -> (isa, body lines)


def add(obj_id, isa, fields):
    objects[obj_id] = (isa, fields)


file_ref = {p: oid("ref", p) for p in all_files}
for p in all_files:
    ext = ".xcassets" if p.endswith(".xcassets") else os.path.splitext(p)[1]
    add(file_ref[p], "PBXFileReference", [
        ("lastKnownFileType", FILE_TYPES[ext]),
        ("path", q(os.path.basename(p))),
        ("sourceTree", '"<group>"'),
    ])

app_product = oid("product", APP)
test_product = oid("product", TESTS)
add(app_product, "PBXFileReference", [
    ("explicitFileType", "wrapper.application"), ("includeInIndex", "0"),
    ("path", f"{APP}.app"), ("sourceTree", "BUILT_PRODUCTS_DIR")])
add(test_product, "PBXFileReference", [
    ("explicitFileType", "wrapper.cfbundle"), ("includeInIndex", "0"),
    ("path", f"{TESTS}.xctest"), ("sourceTree", "BUILT_PRODUCTS_DIR")])

# Build files
app_sources = app_swift + core_cpp
app_build_files = {p: oid("build", APP, p) for p in app_sources}
for p, bid in app_build_files.items():
    add(bid, "PBXBuildFile", [("fileRef", file_ref[p])])
asset_build = oid("build", APP, assets)
add(asset_build, "PBXBuildFile", [("fileRef", file_ref[assets])])
test_build_files = {p: oid("build", TESTS, p) for p in test_swift}
for p, bid in test_build_files.items():
    add(bid, "PBXBuildFile", [("fileRef", file_ref[p])])

# Groups mirror the folder structure.
groups = {}  # folder path -> (id, children list)


def group_for(folder):
    if folder in groups:
        return groups[folder][0]
    gid = oid("group", folder)
    groups[folder] = (gid, [])
    if folder:
        parent = os.path.dirname(folder)
        group_for(parent)
        groups[parent][1].append(gid)
    return gid


for p in all_files:
    folder = os.path.dirname(p)
    group_for(folder)
    groups[folder][1].append(file_ref[p])

products_group = oid("group", "__Products__")
groups["__Products__"] = (products_group, [app_product, test_product])
groups[""][1].append(products_group)

for folder, (gid, children) in groups.items():
    if folder == "":
        fields = [("children", children), ("sourceTree", '"<group>"')]
    elif folder == "__Products__":
        fields = [("children", children), ("name", "Products"), ("sourceTree", '"<group>"')]
    else:
        fields = [("children", children), ("path", q(os.path.basename(folder))), ("sourceTree", '"<group>"')]
    add(gid, "PBXGroup", fields)

# Build phases
app_sources_phase = oid("phase", APP, "sources")
app_frameworks_phase = oid("phase", APP, "frameworks")
app_resources_phase = oid("phase", APP, "resources")
add(app_sources_phase, "PBXSourcesBuildPhase", [
    ("buildActionMask", "2147483647"), ("files", [app_build_files[p] for p in app_sources]),
    ("runOnlyForDeploymentPostprocessing", "0")])
add(app_frameworks_phase, "PBXFrameworksBuildPhase", [
    ("buildActionMask", "2147483647"), ("files", []), ("runOnlyForDeploymentPostprocessing", "0")])
add(app_resources_phase, "PBXResourcesBuildPhase", [
    ("buildActionMask", "2147483647"), ("files", [asset_build]), ("runOnlyForDeploymentPostprocessing", "0")])

test_sources_phase = oid("phase", TESTS, "sources")
test_frameworks_phase = oid("phase", TESTS, "frameworks")
test_resources_phase = oid("phase", TESTS, "resources")
add(test_sources_phase, "PBXSourcesBuildPhase", [
    ("buildActionMask", "2147483647"), ("files", [test_build_files[p] for p in test_swift]),
    ("runOnlyForDeploymentPostprocessing", "0")])
add(test_frameworks_phase, "PBXFrameworksBuildPhase", [
    ("buildActionMask", "2147483647"), ("files", []), ("runOnlyForDeploymentPostprocessing", "0")])
add(test_resources_phase, "PBXResourcesBuildPhase", [
    ("buildActionMask", "2147483647"), ("files", []), ("runOnlyForDeploymentPostprocessing", "0")])

# Targets
project_id = oid("project", APP)
app_target = oid("target", APP)
test_target = oid("target", TESTS)
proxy = oid("proxy", TESTS)
dependency = oid("dependency", TESTS)
add(proxy, "PBXContainerItemProxy", [
    ("containerPortal", project_id), ("proxyType", "1"),
    ("remoteGlobalIDString", app_target), ("remoteInfo", APP)])
add(dependency, "PBXTargetDependency", [("target", app_target), ("targetProxy", proxy)])

common_build = {
    "ALWAYS_SEARCH_USER_PATHS": "NO",
    "CLANG_ANALYZER_NONNULL": "YES",
    "CLANG_CXX_LANGUAGE_STANDARD": '"c++17"',
    "CLANG_CXX_LIBRARY": '"libc++"',
    "CLANG_ENABLE_MODULES": "YES",
    "CLANG_ENABLE_OBJC_ARC": "YES",
    "CLANG_WARN_DOCUMENTATION_COMMENTS": "NO",
    "COPY_PHASE_STRIP": "NO",
    "ENABLE_STRICT_OBJC_MSGSEND": "YES",
    "ENABLE_USER_SCRIPT_SANDBOXING": "YES",
    "GCC_C_LANGUAGE_STANDARD": "gnu17",
    "GCC_NO_COMMON_BLOCKS": "YES",
    "GCC_WARN_64_TO_32_BIT_CONVERSION": "YES",
    "GCC_WARN_UNDECLARED_SELECTOR": "YES",
    "GCC_WARN_UNINITIALIZED_AUTOS": "YES_AGGRESSIVE",
    "GCC_WARN_UNUSED_FUNCTION": "YES",
    "GCC_WARN_UNUSED_VARIABLE": "YES",
    "HEADER_SEARCH_PATHS": '"$(SRCROOT)/Core/include"',
    "MACOSX_DEPLOYMENT_TARGET": DEPLOYMENT,
    "SDKROOT": "macosx",
    "SWIFT_VERSION": "5.0",
}

debug_extra = {
    "DEBUG_INFORMATION_FORMAT": "dwarf",
    "ENABLE_TESTABILITY": "YES",
    "GCC_OPTIMIZATION_LEVEL": "0",
    "GCC_PREPROCESSOR_DEFINITIONS": '("DEBUG=1", "$(inherited)")',
    "ONLY_ACTIVE_ARCH": "YES",
    "SWIFT_ACTIVE_COMPILATION_CONDITIONS": '"DEBUG $(inherited)"',
    "SWIFT_OPTIMIZATION_LEVEL": '"-Onone"',
}
release_extra = {
    "DEBUG_INFORMATION_FORMAT": '"dwarf-with-dsym"',
    "ENABLE_NS_ASSERTIONS": "NO",
    "SWIFT_COMPILATION_MODE": "wholemodule",
    "SWIFT_OPTIMIZATION_LEVEL": '"-O"',
}

app_settings = {
    "ASSETCATALOG_COMPILER_APPICON_NAME": "AppIcon",
    "ASSETCATALOG_COMPILER_GLOBAL_ACCENT_COLOR_NAME": "AccentColor",
    "CODE_SIGN_ENTITLEMENTS": entitlements,
    "CODE_SIGN_IDENTITY": '"-"',
    "CODE_SIGN_STYLE": "Automatic",
    "COMBINE_HIDPI_IMAGES": "YES",
    "CURRENT_PROJECT_VERSION": "1",
    "ENABLE_HARDENED_RUNTIME": "YES",
    "ENABLE_PREVIEWS": "YES",
    "GENERATE_INFOPLIST_FILE": "YES",
    "INFOPLIST_FILE": info_plist,
    "INFOPLIST_KEY_CFBundleDisplayName": "SiEDA",
    "INFOPLIST_KEY_LSApplicationCategoryType": '"public.app-category.developer-tools"',
    "INFOPLIST_KEY_NSHumanReadableCopyright": '"Copyright © 2026 SiEDA. All rights reserved."',
    "LD_RUNPATH_SEARCH_PATHS": '("$(inherited)", "@executable_path/../Frameworks")',
    "MARKETING_VERSION": "1.0",
    "PRODUCT_BUNDLE_IDENTIFIER": BUNDLE_ID,
    "PRODUCT_NAME": '"$(TARGET_NAME)"',
    "SWIFT_EMIT_LOC_STRINGS": "YES",
    "SWIFT_OBJC_BRIDGING_HEADER": '"SiEDA/Bridge/SiEDA-Bridging-Header.h"',
}
test_settings = {
    "BUNDLE_LOADER": '"$(TEST_HOST)"',
    "CODE_SIGN_IDENTITY": '"-"',
    "CODE_SIGN_STYLE": "Automatic",
    "CURRENT_PROJECT_VERSION": "1",
    "GENERATE_INFOPLIST_FILE": "YES",
    "MARKETING_VERSION": "1.0",
    "PRODUCT_BUNDLE_IDENTIFIER": BUNDLE_ID + "Tests",
    "PRODUCT_NAME": '"$(TARGET_NAME)"',
    "SWIFT_EMIT_LOC_STRINGS": "NO",
    "SWIFT_OBJC_BRIDGING_HEADER": '"SiEDA/Bridge/SiEDA-Bridging-Header.h"',
    "TEST_HOST": '"$(BUILT_PRODUCTS_DIR)/SiEDA.app/Contents/MacOS/SiEDA"',
}


def config_list(owner, settings_by_config):
    list_id = oid("configlist", owner)
    ids = []
    for name, settings in settings_by_config:
        cid = oid("config", owner, name)
        ids.append(cid)
        add(cid, "XCBuildConfiguration", [("buildSettings", settings), ("name", name)])
    add(list_id, "XCConfigurationList", [
        ("buildConfigurations", ids), ("defaultConfigurationIsVisible", "0"),
        ("defaultConfigurationName", "Release")])
    return list_id


project_configs = config_list(project_id, [
    ("Debug", {**common_build, **debug_extra}),
    ("Release", {**common_build, **release_extra}),
])
app_configs = config_list(app_target, [("Debug", app_settings), ("Release", app_settings)])
test_configs = config_list(test_target, [("Debug", test_settings), ("Release", test_settings)])

add(app_target, "PBXNativeTarget", [
    ("buildConfigurationList", app_configs),
    ("buildPhases", [app_sources_phase, app_frameworks_phase, app_resources_phase]),
    ("buildRules", []), ("dependencies", []), ("name", APP),
    ("productName", APP), ("productReference", app_product),
    ("productType", '"com.apple.product-type.application"')])
add(test_target, "PBXNativeTarget", [
    ("buildConfigurationList", test_configs),
    ("buildPhases", [test_sources_phase, test_frameworks_phase, test_resources_phase]),
    ("buildRules", []), ("dependencies", [dependency]), ("name", TESTS),
    ("productName", TESTS), ("productReference", test_product),
    ("productType", '"com.apple.product-type.bundle.unit-test"')])

add(project_id, "PBXProject", [
    ("attributes", {
        "BuildIndependentTargetsInParallel": "1",
        "LastSwiftUpdateCheck": "1600",
        "LastUpgradeCheck": "1600",
        "TargetAttributes": {
            app_target: {"CreatedOnToolsVersion": "16.0"},
            test_target: {"CreatedOnToolsVersion": "16.0", "TestTargetID": app_target},
        },
    }),
    ("buildConfigurationList", project_configs),
    ("compatibilityVersion", '"Xcode 14.0"'),
    ("developmentRegion", "en"),
    ("hasScannedForEncodings", "0"),
    ("knownRegions", ["en", "Base"]),
    ("mainGroup", groups[""][0]),
    ("productRefGroup", products_group),
    ("projectDirPath", '""'),
    ("projectRoot", '""'),
    ("targets", [app_target, test_target]),
])

# ---------------------------------------------------------------- serialisation


def fmt(value, indent):
    pad = "\t" * indent
    if isinstance(value, list):
        if not value:
            return "(\n" + pad + ")"
        inner = "".join(f"{pad}\t{v},\n" for v in value)
        return "(\n" + inner + pad + ")"
    if isinstance(value, dict):
        inner = "".join(f"{pad}\t{k} = {fmt(v, indent + 1)};\n" for k, v in sorted(value.items()))
        return "{\n" + inner + pad + "}"
    return str(value)


order = ["PBXBuildFile", "PBXContainerItemProxy", "PBXFileReference", "PBXFrameworksBuildPhase", "PBXGroup",
         "PBXNativeTarget", "PBXProject", "PBXResourcesBuildPhase", "PBXSourcesBuildPhase", "PBXTargetDependency",
         "XCBuildConfiguration", "XCConfigurationList"]

lines = ["// !$*UTF8*$!", "{", "\tarchiveVersion = 1;", "\tclasses = {", "\t};", "\tobjectVersion = 56;", "\tobjects = {", ""]
for isa in order:
    ids = sorted(i for i, (t, _) in objects.items() if t == isa)
    if not ids:
        continue
    lines.append(f"/* Begin {isa} section */")
    for i in ids:
        _, fields = objects[i]
        if isa in ("PBXBuildFile", "PBXFileReference"):
            body = " ".join(f"{k} = {v};" for k, v in [("isa", isa)] + fields)
            lines.append(f"\t\t{i} = {{{body} }};")
        else:
            lines.append(f"\t\t{i} = {{")
            lines.append(f"\t\t\tisa = {isa};")
            for k, v in fields:
                lines.append(f"\t\t\t{k} = {fmt(v, 3)};")
            lines.append("\t\t};")
    lines.append(f"/* End {isa} section */")
    lines.append("")
lines += ["\t};", f"\trootObject = {project_id};", "}", ""]

os.makedirs(PROJECT, exist_ok=True)
with open(os.path.join(PROJECT, "project.pbxproj"), "w") as f:
    f.write("\n".join(lines))

# Workspace settings & shared scheme (needed for `xcodebuild -scheme SiEDA test` in CI).
ws = os.path.join(PROJECT, "project.xcworkspace")
os.makedirs(os.path.join(ws, "xcshareddata"), exist_ok=True)
with open(os.path.join(ws, "contents.xcworkspacedata"), "w") as f:
    f.write('<?xml version="1.0" encoding="UTF-8"?>\n<Workspace\n   version = "1.0">\n'
            '   <FileRef\n      location = "self:">\n   </FileRef>\n</Workspace>\n')
with open(os.path.join(ws, "xcshareddata", "IDEWorkspaceChecks.plist"), "w") as f:
    f.write('<?xml version="1.0" encoding="UTF-8"?>\n<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" '
            '"http://www.apple.com/DTDs/PropertyList-1.0.dtd">\n<plist version="1.0">\n<dict>\n'
            '\t<key>IDEDidComputeMac32BitWarning</key>\n\t<true/>\n</dict>\n</plist>\n')

schemes = os.path.join(PROJECT, "xcshareddata", "xcschemes")
os.makedirs(schemes, exist_ok=True)


def buildable(target_id, name, product):
    return (f'<BuildableReference\n               BuildableIdentifier = "primary"\n'
            f'               BlueprintIdentifier = "{target_id}"\n'
            f'               BuildableName = "{product}"\n'
            f'               BlueprintName = "{name}"\n'
            f'               ReferencedContainer = "container:SiEDA.xcodeproj">\n            </BuildableReference>')


scheme = f'''<?xml version="1.0" encoding="UTF-8"?>
<Scheme
   LastUpgradeVersion = "1600"
   version = "1.7">
   <BuildAction
      parallelizeBuildables = "YES"
      buildImplicitDependencies = "YES">
      <BuildActionEntries>
         <BuildActionEntry
            buildForTesting = "YES"
            buildForRunning = "YES"
            buildForProfiling = "YES"
            buildForArchiving = "YES"
            buildForAnalyzing = "YES">
            {buildable(app_target, APP, APP + ".app")}
         </BuildActionEntry>
      </BuildActionEntries>
   </BuildAction>
   <TestAction
      buildConfiguration = "Debug"
      selectedDebuggerIdentifier = "Xcode.DebuggerFoundation.Debugger.LLDB"
      selectedLauncherIdentifier = "Xcode.DebuggerFoundation.Launcher.LLDB"
      shouldUseLaunchSchemeArgsEnv = "YES">
      <Testables>
         <TestableReference
            skipped = "NO">
            {buildable(test_target, TESTS, TESTS + ".xctest")}
         </TestableReference>
      </Testables>
   </TestAction>
   <LaunchAction
      buildConfiguration = "Debug"
      selectedDebuggerIdentifier = "Xcode.DebuggerFoundation.Debugger.LLDB"
      selectedLauncherIdentifier = "Xcode.DebuggerFoundation.Launcher.LLDB"
      launchStyle = "0"
      useCustomWorkingDirectory = "NO"
      ignoresPersistentStateOnLaunch = "NO"
      debugDocumentVersioning = "YES"
      debugServiceExtension = "internal"
      allowLocationSimulation = "YES">
      <BuildableProductRunnable
         runnableDebuggingMode = "0">
         {buildable(app_target, APP, APP + ".app")}
      </BuildableProductRunnable>
   </LaunchAction>
   <ProfileAction
      buildConfiguration = "Release"
      shouldUseLaunchSchemeArgsEnv = "YES"
      savedToolIdentifier = ""
      useCustomWorkingDirectory = "NO"
      debugDocumentVersioning = "YES">
      <BuildableProductRunnable
         runnableDebuggingMode = "0">
         {buildable(app_target, APP, APP + ".app")}
      </BuildableProductRunnable>
   </ProfileAction>
   <AnalyzeAction
      buildConfiguration = "Debug">
   </AnalyzeAction>
   <ArchiveAction
      buildConfiguration = "Release"
      revealArchiveInOrganizer = "YES">
   </ArchiveAction>
</Scheme>
'''
with open(os.path.join(schemes, f"{APP}.xcscheme"), "w") as f:
    f.write(scheme)

print(f"Generated {PROJECT}: {len(app_swift)} Swift, {len(core_cpp)} C++ sources, {len(test_swift)} test files")
