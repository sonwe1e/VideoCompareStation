# Repository Guidelines

## Agent Start Here

Read [docs/agent-guide.md](docs/agent-guide.md) first for the code/test routing map,
[product intent](docs/product/visual-review.md), and the
[current issue ledger](docs/engineering/visual-review-backlog.md).
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

```powershell
pwsh tools/build/build.ps1 -Preset dev
pwsh tools/build/build.ps1 -Preset release -Test
pwsh tools/build/build.ps1 -Preset dev -Target format-check
pwsh tools/build/build.ps1 -Preset dev -Target lint
pwsh tools/build/build.ps1 -Preset dev -Test -TestRegex 'ui.ImageReviewControllerTests'
```

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

## Coding Style and Naming

Indent C++/QML four spaces, JSON/YAML two, cap C++ at 100 columns. Attached braces,
left-bound pointers, deterministic includes, `clang-format` 19.1.5, `qmlformat`; matching
`clang-tidy` and warning-fatal `qmllint` must pass. A formatter only ever touches the file types it
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
than the manual workaround. An assertion is coverage only if it fails once the behaviour is broken:
attach mutation evidence for every new assertion. A script must assert its own check count so that
a missing anchor cannot turn its checks into silent passes. Read an artifact back before a later
step publishes or consumes it: a scratch path can already hold an older copy, and a publish command
will ship that copy without complaining.
