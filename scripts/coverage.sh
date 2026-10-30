#!/usr/bin/env bash
# Builds the tests with Clang's source-based coverage, runs them, and reports
# how much of c-share's own code (modules/, not the tests or vcpkg's
# libraries) they ran.
#
#   scripts/coverage.sh          the <platform>-coverage preset for this host
#   scripts/coverage.sh <preset> e.g. linux-coverage
#
# Writes to coverage-reports/ (git-ignored):
#   report.txt    the per-file table from llvm-cov, also printed
#   summary.md    the same as a Markdown table, appended to
#                 $GITHUB_STEP_SUMMARY when that is set (GitHub Actions)
#   lcov.info     for tools that read LCOV (e.g. Codecov, IDE gutters)
#   html/         every source line with its count; open html/index.html
#
# Exits non-zero if a test fails, after still writing the reports.
#
# Environment:
#   LLVM_COV, LLVM_PROFDATA   the tools (default: `xcrun <tool>` on macOS,
#                             else from PATH); they must match the compiler's
#                             LLVM version
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$root"

case "$(uname -s)" in
    Darwin) preset="${1:-macos-coverage}" ;;
    Linux) preset="${1:-linux-coverage}" ;;
    *)
        echo "coverage: Clang source-based coverage runs on Linux and macOS only" >&2
        exit 2
        ;;
esac

if [[ "$(uname -s)" == Darwin ]]; then
    llvm_cov=(${LLVM_COV:-xcrun llvm-cov})
    llvm_profdata=(${LLVM_PROFDATA:-xcrun llvm-profdata})
else
    llvm_cov=("${LLVM_COV:-llvm-cov}")
    llvm_profdata=("${LLVM_PROFDATA:-llvm-profdata}")
fi

build_dir="build/$preset"
out_dir="coverage-reports"
profiles="$build_dir/profiles"

cmake --preset "$preset"
cmake --build --preset "$preset" --parallel

# ctest runs each test case in a process of its own: %p keeps their profiles
# apart.
rm -rf "$profiles" "$out_dir"
mkdir -p "$profiles" "$out_dir"
tests_failed=0
LLVM_PROFILE_FILE="$root/$profiles/%p.profraw" ctest --preset "$preset" || tests_failed=1

"${llvm_profdata[@]}" merge -sparse "$profiles"/*.profraw -o "$build_dir/coverage.profdata"

# Every test executable, as llvm-cov takes them: the first plain, the rest
# after -object.
objects=()
for exe in "$build_dir"/bin/*_tests; do
    if [[ ${#objects[@]} -eq 0 ]]; then objects+=("$exe"); else objects+=(-object "$exe"); fi
done
args=(
    "${objects[@]}"
    -instr-profile="$build_dir/coverage.profdata"
    -ignore-filename-regex='(vcpkg_installed|/tests/|^/usr/|^/Library/|^/Applications/)'
)

"${llvm_cov[@]}" report "${args[@]}" | tee "$out_dir/report.txt"
"${llvm_cov[@]}" export -format=lcov "${args[@]}" >"$out_dir/lcov.info"
"${llvm_cov[@]}" show -format=html -output-dir="$out_dir/html" -show-line-counts-or-regions "${args[@]}"
"${llvm_cov[@]}" export -summary-only "${args[@]}" |
    python3 "$root/scripts/coverage_summary.py" "$root/modules/" "$preset" >"$out_dir/summary.md"

if [[ -n "${GITHUB_STEP_SUMMARY:-}" ]]; then
    cat "$out_dir/summary.md" >>"$GITHUB_STEP_SUMMARY"
fi

echo
echo "coverage: reports in $out_dir/ (open $out_dir/html/index.html)"
if [[ $tests_failed -ne 0 ]]; then
    echo "coverage: some tests failed" >&2
    exit 1
fi
