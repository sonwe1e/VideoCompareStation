# Repository Guidelines

## Project Structure and Dependencies

`src/domain` owns rules; `src/application` owns ports. `src/presentation_contract` owns
comparison enums. `platform_windows` provides Windows/D3D11 services. Adapters—`media_ffmpeg`,
`persistence_json`, `ui_qml`, `shell_windows`—implement ports. `src/app` composes; outer types
never enter core. Tests live under `tests/<layer>/<module>`, shared helpers in `tests/support`.

## Build, Test, and Package Commands

Repo scripts require PowerShell 7 (`pwsh`) via `#requires -Version 7.0`; CI runs
`shell: pwsh`. Windows PowerShell 5.1 is unsupported. Machine-local tool paths (vcpkg, MSVC
`vcvarsall.bat`, ninja) live in `tools/build/env.ps1`, overridable via parameters/environment.
Use the wrapper:

```powershell
pwsh tools/build/build.ps1 -Preset dev
pwsh tools/build/build.ps1 -Preset release -Test
pwsh tools/build/build.ps1 -Preset dev -FormatCheck -Lint
pwsh tools/build/build.ps1 -Preset dev -Test -TestRegex 'ui.ImageReviewControllerTests'
```

Raw preset commands remain valid inside the wrapper's vcvarsall shell:

```powershell
cmake --preset dev
cmake --build --preset dev
ctest --preset dev --output-on-failure
.\out\build\dev\bin\VCStationCli.exe --startup-check
cmake --build --preset dev --target format-check
cmake --build --preset dev --target lint
cmake --preset release
cmake --build --preset release
cpack --preset release-zip
cpack --preset release-msi
```

Keep outputs in `out/`.

## Coding Style and Naming

Indent C++/QML four spaces, JSON/YAML two, cap C++ at 100 columns. Attached braces,
left-bound pointers, deterministic includes, `clang-format` 19.1.5, `qmlformat`; matching
`clang-tidy` and warning-fatal `qmllint` must pass. C++ types and files use `PascalCase`,
functions and variables `lowerCamelCase`, constants `kPascalCase`, namespaces `dvs::<module>`.
QML components use `PascalCase.qml`; IDs and properties `lowerCamelCase`.

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
transactionally. Preserve approved tests and performance gates.
