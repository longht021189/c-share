#!/usr/bin/env bash
# Lints c-share's C/C++: clang-format (.clang-format), then clang-tidy
# (.clang-tidy), then -- only when CLION_INSPECT_SH_PATH is set -- CLion's own
# inspections (.idea/inspectionProfiles/Project_Default.xml).
#
#   scripts/lint.sh          check; exits non-zero on any finding
#   scripts/lint.sh --fix    reformat in place, then check the rest
#
# clang-tidy reads compile_commands.json from a configured build: run
# `cmake --preset <platform>` first. Only sources that build there are
# checked, so e.g. waker_epoll.cpp is checked on Linux and waker_kqueue.cpp on
# macOS.
#
# Environment:
#   CLANG_FORMAT, CLANG_TIDY   the tools (default: from PATH)
#   BUILD_DIR                  the configured build (default: build/<preset>)
#   CLION_INSPECT_SH_PATH      CLion's inspect.sh, e.g.
#                              /Applications/CLion.app/Contents/bin/inspect.sh;
#                              unset skips CLion's inspections. It runs with
#                              config/system directories of its own, so CLion
#                              may stay open.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$root"

fix=0
case "${1:-}" in
    --fix) fix=1 ;;
    "") ;;
    *)
        echo "usage: scripts/lint.sh [--fix]" >&2
        exit 2
        ;;
esac

clang_format="${CLANG_FORMAT:-clang-format}"
clang_tidy="${CLANG_TIDY:-clang-tidy}"
for tool in "$clang_format" "$clang_tidy"; do
    if ! command -v "$tool" >/dev/null; then
        echo "lint: $tool not found (set CLANG_FORMAT / CLANG_TIDY to its path)" >&2
        exit 2
    fi
done

case "$(uname -s)" in
    Darwin) default_build=build/macos ;;
    Linux) default_build=build/linux ;;
    *) default_build=build/windows ;;
esac
build_dir="${BUILD_DIR:-$default_build}"
if [[ ! -f "$build_dir/compile_commands.json" ]]; then
    echo "lint: no $build_dir/compile_commands.json; run \`cmake --preset <platform>\` first" >&2
    exit 2
fi

failed=0

# --- clang-format: every C/C++ file in git.
sources=()
while IFS= read -r file; do sources+=("$file"); done < <(
    git ls-files '*.c' '*.cpp' '*.h' '*.hpp'
)
if ((fix)); then
    "$clang_format" -i "${sources[@]}"
elif ! "$clang_format" --dry-run --Werror "${sources[@]}"; then
    echo "lint: clang-format found unformatted code; run scripts/lint.sh --fix" >&2
    failed=1
fi

# --- clang-tidy: the translation units this build compiles. Its fixes are
# not applied automatically: a header included by several files gets the same
# fix more than once, which breaks it.
units=()
while IFS= read -r file; do units+=("$file"); done < <(
    python3 -c '
import json, os, sys
root = sys.argv[2]
for entry in json.load(open(sys.argv[1])):
    path = os.path.realpath(os.path.join(entry["directory"], entry["file"]))
    rel = os.path.relpath(path, root)
    if rel.startswith(("modules/", "examples/", "tests/")):
        print(rel)
' "$build_dir/compile_commands.json" "$(pwd -P)" | sort -u
)
tidy_args=(-p "$build_dir" --quiet)
if [[ "$(uname -s)" == Darwin ]] && command -v xcrun >/dev/null; then
    # A clang-tidy that is not Apple's (e.g. CLion's, or LLVM's) knows neither
    # the macOS SDK nor where its compiler's builtin headers are, and without
    # them it reports nonsense (methods that "can be static", ...).
    tidy_args+=("--extra-arg=-isysroot$(xcrun --show-sdk-path)")
    tidy_args+=("--extra-arg=-resource-dir=$(xcrun clang -print-resource-dir)")
fi
if ((${#units[@]})) && ! "$clang_tidy" "${tidy_args[@]}" "${units[@]}"; then
    echo "lint: clang-tidy found problems" >&2
    failed=1
fi

# --- CLion's inspections, only when asked for.
if [[ -n "${CLION_INSPECT_SH_PATH:-}" ]]; then
    if [[ ! -x "$CLION_INSPECT_SH_PATH" ]]; then
        echo "lint: CLION_INSPECT_SH_PATH=$CLION_INSPECT_SH_PATH is not executable" >&2
        exit 2
    fi
    # Reports go to lint-reports/clion (git-ignored): one JSON per inspection
    # that found something, and summary.md listing them all by file.
    out="$root/lint-reports/clion"
    rm -rf "$out"
    # inspect.sh refuses to start next to a running CLion that uses the same
    # config and system directories, so it gets its own, seeded with the
    # user's license and settings (toolchains, CMake profiles).
    case "$(uname -s)" in
        Darwin) jetbrains="$HOME/Library/Application Support/JetBrains" ;;
        Linux) jetbrains="${XDG_CONFIG_HOME:-$HOME/.config}/JetBrains" ;;
        *) jetbrains="${APPDATA:-}/JetBrains" ;;
    esac
    user_config="$(ls -d "$jetbrains"/CLion* 2>/dev/null | sort -V | tail -n 1 || true)"
    home="$root/$build_dir/clion-inspect-home"
    rm -rf "$home"
    mkdir -p "$home/config" "$home/system" "$home/log"
    if [[ -n "$user_config" ]]; then
        cp "$user_config"/*.key "$home/config/" 2>/dev/null || true
        [[ -d "$user_config/options" ]] && cp -R "$user_config/options" "$home/config/"
    fi
    cat >"$home/idea.properties" <<EOF
idea.config.path=$home/config
idea.system.path=$home/system
idea.log.path=$home/log
EOF
    # Its exit status is not reliable: it fails after writing a complete report
    # when it cannot convert its own descriptions file. A report directory
    # without .descriptions.json is what an actual failure leaves.
    CLION_PROPERTIES="$home/idea.properties" "$CLION_INSPECT_SH_PATH" "$root" \
        "$root/.idea/inspectionProfiles/Project_Default.xml" "$out" -v1 -format json || true
    if [[ ! -f "$out/.descriptions.json" ]]; then
        echo "lint: inspect.sh produced no report (see $home/log)" >&2
        exit 2
    fi
    # One <inspection>.json per inspection that found something. Only what
    # is found in this repo's own files counts: CLion also inspects what the
    # CMake project loads from build/, vcpkg and vcpkg-share.
    if ! git ls-files | python3 -c '
import collections, glob, json, os, sys

out, root = sys.argv[1], sys.argv[2]
owned = set(sys.stdin.read().split("\n"))
by_file = collections.defaultdict(list)
counts = collections.Counter()
skipped = 0
for report in sorted(glob.glob(os.path.join(out, "*.json"))):
    for problem in json.load(open(report)).get("problems", []):
        path = problem.get("file", "").replace("file://$PROJECT_DIR$/", "")
        if path not in owned:
            skipped += 1
            continue
        inspection = problem["problem_class"]["id"]
        counts[inspection] += 1
        by_file[path].append((problem.get("line") or 0, inspection, problem.get("description", "")))
lines = ["# CLion inspections", ""]
if counts:
    lines += ["| Inspection | Problems |", "|---|---|"]
    lines += [f"| {name} | {count} |" for name, count in counts.most_common()]
else:
    lines.append("No problems in this repo'"'"'s files.")
lines += ["", f"{skipped} more outside them (build/, vcpkg, vcpkg-share) are not listed."]
for path in sorted(by_file):
    lines += ["", f"## {path}", ""]
    for line, inspection, description in sorted(by_file[path]):
        description = " ".join(description.split())
        lines.append(f"- [{path}:{line}]({os.path.relpath(os.path.join(root, path), out)}#L{line}) "
                     f"`{inspection}` {description}")
open(os.path.join(out, "summary.md"), "w").write("\n".join(lines) + "\n")
if counts:
    print(f"lint: CLion'"'"'s inspections found {sum(counts.values())} problems:", file=sys.stderr)
    for name, count in counts.most_common():
        print(f"  {count:5}  {name}", file=sys.stderr)
    sys.exit(1)
' "$out" "$root"; then
        echo "lint: see $out/summary.md" >&2
        failed=1
    fi
fi

exit "$failed"
