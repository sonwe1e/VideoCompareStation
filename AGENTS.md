# Repository Guidelines

## Agent Start Here

Read [docs/agent-guide.md](docs/agent-guide.md) first for the code/test routing map,
[product intent](docs/product/visual-review.md), the
[current issue ledger](docs/engineering/visual-review-backlog.md), and the
[decision register](docs/engineering/decision-register.md) for which side a settled
dispute landed on and whether it is still a temporary assumption.
The product covers silent video/image viewing and quality comparison; audio and subtitles
are outside the current scope. Check HEAD and the working-tree diff before treating an issue
as open or fixed. Older playback plans describe earlier baselines, not current completion status.

## Project Structure and Dependencies

`src/domain` owns rules; `src/application` owns ports. `src/presentation_contract` owns
comparison enums. `platform_windows` provides Windows/D3D11 services. Adapters—`media_ffmpeg`,
`persistence_json`, `ui_qml`, `shell_windows`—implement ports. `src/app` composes; outer types
never enter core. Tests live under `tests/<layer>/<module>`, shared helpers in `tests/support`.

## Build, Test, and Package Commands

Repo scripts require PowerShell 7 (`pwsh`) via `#requires -Version 7.0`; CI runs
`shell: pwsh`. Windows PowerShell 5.1 is unsupported. Tool discovery and overrides are
centralized in `tools/build/env.ps1`; do not hardcode machine paths. See
[docs/building.md](docs/building.md) for diagnostics and cache recovery.
Use the wrapper:

Local build/check/test work must use this wrapper, including single-target probes. It limits the
process tree to at most four logical CPUs, limits compilation to two jobs at BelowNormal priority,
and serializes all presets in this checkout. Tests keep Normal priority inside the same CPU bound
so scheduler starvation does not invalidate UI/shutdown deadlines.
Do not bypass it with raw Ninja or hand-built PATH/cmd invocations: Ninja defaults
to CPU-count-based parallelism, and missing VSLANG can corrupt incremental header dependencies.
Generated compile/link pools are additional protection, not a substitute for the wrapper.

```powershell
pwsh tools/build/build.ps1 -Preset dev
pwsh tools/build/build.ps1 -Preset release -Test
pwsh tools/build/build.ps1 -Preset dev -Target format-check
pwsh tools/build/build.ps1 -Preset dev -Target lint
pwsh tools/build/build.ps1 -Preset dev -Test -TestRegex 'ui.ImageReviewControllerTests'
pwsh tools/build/build.ps1 -Preset dev -Test -TestJobs 2
pwsh tools/build/build.ps1 -Preset dev -Test -TestGroup Sidebar -TestJobs 2
```

The preset keeps `CTEST_PARALLEL_LEVEL=1` for CI's low CPU impact; `-TestJobs N` is the local
opt-in (`RUN_SERIAL` UI tests still run one at a time). Targeted `-TestRegex` runs remain the
default dev loop. `-TestGroup Core|Media|Images|Playback|Sidebar|Ui|Build|Shell` provides named
development routes and runs one representative UI style/DPI per suite; add `-FullMatrix` for
style, popup, layout or DPI changes. Groups are starting points, not automatic impact analysis:
use `-TestRegex` for a narrower case or additional affected suites. Do not combine both selectors.

Use impact-based validation for both iteration and handoff; a small, isolated change may finish
with relevant tests and applicable checks, without a full suite or whole-repository lint.
Choose scope from changed behavior and callers, not line count. See the required selection table
in [docs/building.md](docs/building.md#开发循环与完整验收).
For local C++ changes, build with warning-fatal compilation, run the affected suite and C++ format
check; full clang-tidy may be deferred only for low-risk changes that do not alter ownership,
lifetime, concurrency, public contracts or data integrity. QML-only changes use the existing QML
format/lint targets and affected behavior tests; retain affected style/DPI variants for layout changes.
Shared contracts, broad cross-layer changes, toolchain/global build changes, unclear impact, and
release acceptance require the full suite and applicable format/lint gates once after stabilization.
Critical behavior changes also retain targeted mutation/performance evidence where applicable.
CI and release gates are unchanged; `-Test` without a selector still runs the complete preset.
After a failure, rerun the failing scope first. Do not repeat a successful gate on identical inputs
without a new concern. Documentation-only changes need applicable documentation/script checks,
not a media/UI run. Report selected scope and deferred checks briefly; never call a subset a full pass.
Diagnose prolonged silent checks using the current job log and process activity over time; a live
process or cumulative CPU time alone does not prove progress. Do not repeatedly read stale job logs.

Raw preset commands require a configured MSVC x64 developer environment:

```powershell
cmake --preset dev
cmake --build --preset dev
ctest --preset dev --output-on-failure
.\out\build\dev\bin\CompareStationCli.exe --startup-check
cmake --build --preset dev --target format-check
cmake --build --preset dev --target lint
cmake --preset release
cmake --build --preset release
cpack --preset release-zip
pwsh tools/release/verify-release-package.ps1
pwsh tools/shell/RegisterExplorerCommand.ps1 -InstallRoot out\build\release\bin
```

Keep outputs in `out/`. Verification harnesses live in the repository test suite
(`tests/<layer>/<module>`); `out/verification/` holds one-off probes, mutation scripts and run logs,
never a second copy of a suite. `pwsh tools/quality/check-script-integrity.ps1` and
`pwsh tools/quality/check-hardcoded-paths.ps1` run inside `ctest` and keep script files and machine
paths honest. Moving or renaming the checkout needs `-Fresh`; see [docs/building.md](docs/building.md).

The ZIP is the only published package; there is no MSI to maintain. Installing means unpacking the
ZIP: on startup the application itself registers the Explorer "Compare with CompareStation" command
for the current user under `HKCU\Software\Classes`, repairs it when the directory moves, and records
the user's own choice in `HKCU\Software\CompareStation\ExplorerContextMenu` so that `-Uninstall`
stays in effect; `DVS_DISABLE_SHELL_REGISTRATION=1` skips the check. The script
`tools/shell/RegisterExplorerCommand.ps1` does the same job on demand for unattended installs, and
`RegisterCompareStationContextMenu.cmd` is the double-click entry point for users who do not open a
terminal. It covers five video and thirteen still-image extensions: one file opens for review, two
videos or two images open as a comparison, and any other selection - a directory, a network path, a
mixed pair, three images - keeps the command hidden. `-Uninstall` removes the keys again.

Package only the runtime dependencies, resources, licenses, and user entry points the current
product needs. Automatic Qt deployment is not an approval to ship extra installers, unused graphics
compilers, tools, symbols, or test media. Keep required app-local CRT/UCRT DLLs, enforce the CPack
runtime-payload gate, and explain per-file additions against the preceding ZIP before release.
See [the packaging policy](docs/building.md#zip-只打包必要运行时) for evidence and validation requirements.

## Coding Style and Naming

Indent C++/QML four spaces, JSON/YAML two, cap C++ at 100 columns. Attached braces,
left-bound pointers, deterministic includes, `clang-format` 19.1.5, `qmlformat`; matching
`clang-tidy` and warning-fatal `qmllint` must pass when required by the validation scope above.
A formatter only ever touches the file types it
owns - run them through the `format-check` and `lint` targets rather than by hand, because
clang-format reads an unknown extension as C++ and has destroyed a `.ps1` that way. C++ types and
files use `PascalCase`, functions and variables `lowerCamelCase`, constants `kPascalCase`,
namespaces `dvs::<module>`. QML components use `PascalCase.qml`; IDs and properties
`lowerCamelCase`.

## Testing and Performance

Use GoogleTest for C++ and Qt Quick Test for UI. Name C++ tests `ThingTests.cpp`, QML tests
`tst_thing.qml`; label CTest cases by layer and module. `domain` and `application`
suites require 80% line coverage.

On D3D11VA runners, 2-3 source 1080p60 playback runs five minutes, excluding warm-up and
seek/pause intervals. Never split a frame set across sources. Limits: 0.5% set-level frame
drops, 500 ms seek P95, 100 ms UI response, 256 MiB decoded-frame cache.

## Commits, Pull Requests, and Agent Invariants

Use Conventional Commits subjects. PRs explain changes, link issues, and list tests; add
performance evidence for media/render work and screenshots for visible QML changes.

Never block GUI/render threads, publish partial frame sets, or omit session/generation/request
identity on asynchronous work. Hide FFmpeg/D3D11 types behind adapters. Write files
transactionally. Preserve approved tests and performance gates. Fix the product behaviour rather
than the manual workaround. An assertion is coverage only if it can detect broken behaviour.
Attach targeted mutation evidence per changed critical behaviour (async identity, complete frame
sets, timing, cache limits, data integrity), not per assertion. Related assertions may share one
counterexample. For low-risk presentation changes, focused behaviour tests and applicable visual
checks suffice; test-routing changes must prove that selection cannot silently omit its gates.
A script must assert its own check count so that
a missing anchor cannot turn its checks into silent passes. Read an artifact back before a later
step publishes or consumes it: a scratch path can already hold an older copy, and a publish command
will ship that copy without complaining.
