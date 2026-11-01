# c-share

Cross-platform C/C++ static library project, built with CMake, its
dependencies declared in [vcpkg-share](https://github.com/longht021189/vcpkg-share)'s
vcpkg manifest.

## Design decisions

- **C++17 max.** The project targets Android via the NDK alongside
  desktop platforms, so the language standard is capped at C++17
  (`CMakeLists.txt` sets `CMAKE_CXX_STANDARD 17`).
- **Static libraries only.** `BUILD_SHARED_LIBS` is forced `OFF` at the root;
  every module builds a `STATIC` library. Objects are still built
  position-independent (`CMAKE_POSITION_INDEPENDENT_CODE ON`) so the static
  libs can be linked into a shared object later (e.g. an Android `.so`).
- **Manifest-mode vcpkg, manifest in vcpkg-share.** Dependencies live in
  vcpkg-share's `vcpkg.json` (checked out next to c-share, or at
  `VCPKG_SHARE_PATH`), resolved per-build into
  `build/<preset>/vcpkg_installed/` — nothing is installed globally. A
  consumer can instead build against vcpkg-share's prebuilt prefix
  (`-DCMAKE_PREFIX_PATH=<prefix>`, no vcpkg toolchain), as r-share's `build.rs`
  does with `VCPKG_SHARE_PREFIX`.
- **One module = one CMakeLists.txt.** Each module under `modules/` is a
  self-contained static library target with its own `CMakeLists.txt`,
  `include/`, and `src/`. The root `CMakeLists.txt` only wires modules
  together; it never lists module sources directly.

## Layout

```
CMakeLists.txt          # root: global settings, adds modules/examples/tests
CMakePresets.json        # vcpkg toolchain + per-platform presets
cmake/
  CompilerOptions.cmake  # shared per-target compile settings helper
modules/
  CMakeLists.txt         # adds each module subdirectory
  waker/                 # cross-platform readiness primitive (epoll/kqueue/WFMO)
    CMakeLists.txt
    include/c_share/waker/
    src/
  http/                   # non-blocking HTTP client (libcurl is an internal detail), built on waker
    CMakeLists.txt
    include/c_share/http/
    src/
  crypto/                 # plain C API (wolfSSL-backed) for bindgen/other-language FFI
    CMakeLists.txt
    include/c_share/crypto/
    src/
  evm/                    # EVM JSON-RPC client (reads, contract calls, signed transactions), on http and crypto
    CMakeLists.txt
    include/c_share/evm/
    src/
examples/
  waker_demo/             # small executable exercising waker's 3 wake-up kinds
tests/                    # Catch2 unit tests, one executable per module
  CMakeLists.txt
  waker/ crypto/ http/ evm/
  support/                # loopback sockets and a scriptable HTTP/WebSocket server
scripts/
  lint.sh                 # clang-format, clang-tidy, CLion's inspections
  coverage.sh             # tests with coverage, reports in coverage-reports/
  check_dependencies.py   # known vulnerabilities in the vcpkg packages (OSV)
  check_rpc_endpoints.py  # whether evm's public RPC URLs still answer
.github/
  workflows/              # tests.yml, security.yml, lint.yml (see "Continuous integration")
  actions/setup-vcpkg/    # vcpkg at the pinned commit, with its binary cache
```

Each module exposes an `ALIAS` target under the `c_share::` namespace
(e.g. `c_share::waker`), so consumers always do
`target_link_libraries(<target> PRIVATE c_share::waker)` rather than
referencing the raw target name.

## Prerequisites

- CMake ≥ 3.23
- A C++17 compiler (MSVC, Clang, GCC, or Android NDK's Clang)
- [vcpkg](https://github.com/microsoft/vcpkg), with `VCPKG_ROOT` set in your
  environment, at the commit vcpkg-share's `VCPKG_COMMIT` names
- [vcpkg-share](https://github.com/longht021189/vcpkg-share) next to this
  checkout (`../vcpkg-share`), or at `VCPKG_SHARE_PATH`
- Android NDK (only for Android presets), with `ANDROID_NDK_HOME` set

## Building

Presets in `CMakePresets.json` set the vcpkg toolchain file and pick a
static-library triplet per platform; `CMakeLists.txt` points vcpkg at
vcpkg-share's manifest, overlay ports and triplets.

```sh
# Desktop (picks linux / macos / windows automatically via the preset condition)
cmake --preset linux      # or: macos / windows
cmake --build --preset linux
ctest --preset linux      # the unit tests

# Android (arm64-v8a; also android-armv7, android-x86_64, android-x86)
cmake --preset android-arm64
cmake --build --preset android-arm64
```

Useful options (pass with `-D` at configure time):

- `-DC_SHARE_BUILD_EXAMPLES=OFF` — skip building `examples/` (on by default)
- `-DC_SHARE_BUILD_TESTS=ON` — build `tests/`. Off by default; the `linux`,
  `macos` and `windows` presets turn it on. It also adds vcpkg-share's `tests` feature,
  which brings in Catch2, so a consumer that builds c-share without tests
  (e.g. r-share's `build.rs`) never installs or links it
- `-DC_SHARE_BUILD_HTTP=OFF` — skip `modules/http` (on by default), so a consumer
  that only wants `c_share::waker` isn't forced through `find_package(CURL)`
  and vcpkg's libcurl/wolfSSL resolution
- `-DC_SHARE_BUILD_CRYPTO=OFF` — skip `modules/crypto` (on by default); still
  needs vcpkg's wolfSSL resolution when left on, same as `http`
- `-DC_SHARE_BUILD_EVM=OFF` — skip `modules/evm` (on by default); it builds
  on `http` and `crypto`, so it is off whenever either of them is

## Linting

```sh
scripts/lint.sh        # check: clang-format, then clang-tidy
scripts/lint.sh --fix  # reformat in place first
```

- `.clang-format` and `.clang-tidy` hold the rules, and CLion uses both
  files too. `.clang-tidy` says why each disabled check is off.
- clang-tidy needs a configured build (`cmake --preset <platform>`) for its
  `compile_commands.json`, and checks only what that build compiles.
- The tools come from `PATH`, or from `CLANG_FORMAT`/`CLANG_TIDY`. CLion ships
  a clang-tidy (`/Applications/CLion.app/Contents/bin/clang/mac/aarch64/bin/`)
  but no clang-format: `pip install clang-format` or `brew install
  clang-format`.
- Its fixes are never applied with `clang-tidy --fix`: a header included by
  several files gets the same fix more than once, which breaks it.

With `CLION_INSPECT_SH_PATH` set to CLion's `inspect.sh`
(`/Applications/CLion.app/Contents/bin/inspect.sh` on macOS), the script also
runs CLion's own inspections, with the profile in
`.idea/inspectionProfiles/Project_Default.xml`, and writes their reports to
`lint-reports/clion` (git-ignored): a JSON per inspection that found something,
and `summary.md` with every problem by file. Only problems in this repo's own
files count; CLion also inspects what the CMake project loads from `build/`
vcpkg and vcpkg-share. CLion may stay
open: `inspect.sh` runs with config and system directories of its own (under
the build dir), seeded with CLion's license and settings.

- The profile is CLion's defaults, minus `CppUseDesignatedInitializers`
  (designated initializers are C++20, this code is C++17). To share a
  customized one, change it in Settings → Editor → Inspections (profile
  *Project Default*), or export it from the ⚙ menu there into that file, and
  commit it. The rest of `.idea/` stays out of git, except the spelling
  dictionary, `.idea/dictionaries/project.xml`.
- A finding that is wrong for one place is silenced there, with the reason in
  a comment above: `// ReSharper disable once <Inspection>` for the next line,
  or `// ReSharper disable <Inspection>` ... `// ReSharper restore
  <Inspection>` around several (`//noinspection` does not work for C++).
  Examples: libcurl's callback prototypes, the `#include`s of what a file
  uses that CLion finds through other headers, and the pimpl classes'
  methods, which CLion would all make `const`.

## Testing

`ctest --preset <platform>` runs every test; each module's executable
(`build/<platform>/bin/{waker,crypto,http}_tests`) also runs alone and takes
Catch2's options, e.g. `http_tests "[websocket]"` or `http_tests --list-tests`.

- `waker_tests`: wakes, timers and socket readiness over loopback socket pairs.
- `crypto_tests`: RFC 7748/8032/5869 test vectors, round trips and tampering
  for every function, secp256k1/EVM against the well-known keys 1 and
  n - 1, and secp256k1 refusing private keys out of range.
- `http_tests`: `Client` and the C API against `tests/support/test_server`, a
  loopback server each test scripts (requests, redirects, gzip, timeouts,
  cancellation, WebSocket handshakes, frames, permessage-deflate), and the
  WebSocket codec against RFC 6455/7692's example frames, and timers.
- `evm_tests`: the Solidity ABI specification's examples, EIP-55 addresses,
  RLP, EIP-1559 transactions signed byte-for-byte as eth-account signs them,
  the client against a loopback JSON-RPC node each test scripts, and the
  public network table (offline; `scripts/check_rpc_endpoints.py` checks the
  endpoints themselves).

They need no network: everything runs on 127.0.0.1. `tests/.clang-tidy`
turns off two checks Catch2's macros trip (see the file).

### Coverage

`scripts/coverage.sh` builds the tests with Clang's source-based coverage
(the `linux-coverage`/`macos-coverage` presets, which set
`C_SHARE_COVERAGE`), runs them, and prints how much of `modules/` they ran.
The reports land in `coverage-reports/` (git-ignored): `report.txt`,
`summary.md`, `lcov.info` and `html/index.html`, every line with its count.
On Linux it needs `clang` and the matching `llvm-cov`/`llvm-profdata`
(e.g. Ubuntu's `clang` and `llvm` packages); on macOS, Xcode's. GitHub
Actions runs it on both (see [Continuous integration](#continuous-integration)).

### Sanitizers

The `linux-asan`, `linux-tsan` and `macos-asan` presets build the tests with
Clang's sanitizers (`C_SHARE_SANITIZE`); their test presets set the
sanitizers' options, so any finding fails the test that hit it:

```sh
cmake --preset linux-asan && cmake --build --preset linux-asan && ctest --preset linux-asan
```

- `linux-asan`: AddressSanitizer (out-of-bounds, use-after-free, double
  free), LeakSanitizer (memory leaks) and UndefinedBehaviorSanitizer.
- `linux-tsan`: ThreadSanitizer (data races, lock-order inversions).
- `macos-asan`: ASan and UBSan; LeakSanitizer does not run on macOS, so leaks
  are checked on Linux only.

Only c-share's own targets are instrumented, not the vcpkg packages.

## Continuous integration

Every check below is free for a public repository. Not all of them need to
stay: the "Suggested" column is a recommendation, and a job can be removed
from its workflow (or its trigger narrowed) without touching the others.

| Workflow / job                                   | What it does                                                                                                                                      | Reports to                                                                                               | Suggested                                                                                                         |
|--------------------------------------------------|---------------------------------------------------------------------------------------------------------------------------------------------------|----------------------------------------------------------------------------------------------------------|-------------------------------------------------------------------------------------------------------------------|
| `tests.yml` / `linux-coverage`, `macos-coverage` | Unit tests with coverage (`scripts/coverage.sh`), on every push to `main` and pull request                                                        | Job log and summary; `coverage-<preset>` artifact                                                        | **Keep**: the tests on both desktop platforms, plus coverage                                                      |
| `tests.yml` / `linux-asan`                       | Tests under ASan + LSan + UBSan                                                                                                                   | Fails the job with the stack trace                                                                       | **Keep**: the only memory-leak check that runs on every change                                                    |
| `tests.yml` / `linux-tsan`                       | Tests under TSan                                                                                                                                  | Fails the job                                                                                            | **Keep**: `Client`, the wakers and the tests are multi-threaded                                                   |
| `tests.yml` / `macos-asan`                       | Tests under ASan + UBSan on macOS                                                                                                                 | Fails the job                                                                                            | Optional: mostly repeats `linux-asan`, but covers the kqueue waker                                                |
| `tests.yml` / `valgrind`                         | Tests under Valgrind's memcheck; only from "Run workflow" with *valgrind* ticked                                                                  | Fails the job                                                                                            | Optional: ASan/LSan find the same and more, 20-50x faster; Valgrind's slowness can trip the tests' short timeouts |
| `security.yml` / `codeql`                        | CodeQL's `security-extended` queries over c-share's code, on every push, pull request and daily                                                   | Security tab, Code scanning; pull request annotations                                                    | **Keep**                                                                                                          |
| `security.yml` / `dependencies`                  | Known vulnerabilities in the vcpkg packages (`scripts/check_dependencies.py`), on every push, pull request and daily                              | Security tab, Code scanning (one alert per advisory, closed once the version moves past it); job summary | **Keep**: nothing else sees vcpkg packages' advisories                                                            |
| `lint.yml` / `clang`                             | `scripts/lint.sh`'s clang-format and clang-tidy, on every push to `main` and pull request; CLion's inspections need a license, so they stay local | Fails the job with the findings                                                                          | **Keep**: CLion's suggestions aside, the same bar as local lint                                                   |
| `lint.yml` / `actionlint`                        | Checks the workflows themselves                                                                                                                   | Fails the job                                                                                            | **Keep**: seconds per run                                                                                         |

Considered and left out:

- **Dependabot, OSV-Scanner, Trivy, Grype** cannot see vcpkg packages:
  GitHub's advisory database has no vcpkg ecosystem, and vcpkg's SBOMs
  (`vcpkg.spdx.json`) have no PURL or CPE that these scanners match on.
  OSV-Scanner 2.7.0 reported "No issues found" for curl 8.22.0 while OSV
  listed 20 advisories for it. `scripts/check_dependencies.py` asks OSV by
  the upstream commit instead.
- **Fuzzing** (ClusterFuzzLite, free): suggested next, for the code that
  parses what a server sends: `ws_codec`'s decoder and
  `parse_ws_extensions`. It needs fuzz targets written first.
- **Windows**: no job yet; the tests have not been built there.

Suggested branch protection for `main`: require the `linux-coverage`,
`macos-coverage`, `linux-asan`, `linux-tsan`, `clang`, `actionlint` and
`codeql` checks. Leave
`dependencies` out of the required checks: it fails as soon as an upstream
advisory appears, even when the change under review has nothing to do with
it.

### Dependency vulnerabilities

`python3 scripts/check_dependencies.py build/<preset>` checks a configured
build's vcpkg packages against [OSV](https://osv.dev) and exits 1 if any has
a known vulnerability. Not every advisory applies to c-share's build (curl
has no HTTP/2 or FTP here, for example); dismiss those in the Security tab
with a reason, and they stay dismissed.

## Adding a module

1. `modules/<name>/CMakeLists.txt`, `include/c_share/<name>/`, `src/`.
2. Define a `STATIC` library target and an `c_share::<name>` alias, following
   `modules/waker/CMakeLists.txt` as a template.
3. Add `add_subdirectory(<name>)` to `modules/CMakeLists.txt`.
4. Add a `<name>_tests` executable to `tests/CMakeLists.txt`.

## Dependencies

The vcpkg dependencies (curl, wolfSSL, zlib, nlohmann-json; Catch2 for the
tests) are declared in [vcpkg-share](https://github.com/longht021189/vcpkg-share),
with its overlay ports and triplets, and the vcpkg commit they build against.
See its README for adding one, and for why curl is pinned to wolfSSL.

