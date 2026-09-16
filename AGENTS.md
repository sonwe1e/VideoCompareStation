# Repository Guidelines

## Project Structure and Dependencies

`src/domain` owns rules; `src/application` owns ports. `src/presentation_contract` owns
comparison enums. `platform_windows` provides Windows/D3D11 services.
Adapters—`media_ffmpeg`, `persistence_json`, `ui_qml`, `shell_windows`—implement ports.
`src/app` composes; outer types never enter core.
`tests/smoke` contains startup checks; place other tests under `tests/<layer>/<module>` and
shared support under `tests/support`.
`assets/`, `packaging/`, `docs/`, and `licenses/` hold runtime data, distribution,
architecture notes, and notices.

## Build, Test, and Package Commands

PowerShell 7 (`pwsh`) is the required shell — every repository `.ps1`/`.psm1` carries
`#requires -Version 7.0`, and CI runs `shell: pwsh`. Windows PowerShell 5.1 is not
supported for repo scripts.

Machine-local build defaults (vcpkg, MSVC/vcvarsall, ninja) live in one place,
`tools/build/env.ps1`, with this machine's paths as defaults and parameters/
environment as overrides:
- `VCPKG_ROOT` default `G:\Workspaces\vcpkg` (an invalid existing `$env:VCPKG_ROOT`
  is ignored with a warning rather than trusted);
- MSVC via the VS 2022 BuildTools `vcvarsall.bat`
  (`C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvarsall.bat`,
  with Community/Professional/Enterprise fallbacks);
- Ninja from `C:\msys64\ucrt64\bin` prepended to `PATH` when not already found.

Use the unified wrapper instead of hand-rolled shell:

```powershell
pwsh tools/build/build.ps1 -Preset dev                       # build a preset
pwsh tools/build/build.ps1 -Preset release -Test             # build + full ctest
pwsh tools/build/build.ps1 -Preset dev -FormatCheck -Lint    # quality gates
pwsh tools/build/build.ps1 -Preset dev -Test -TestRegex 'ui.ImageReviewControllerTests'
```

It sources `env.ps1`, auto-configures when the build dir has no CMake cache, and runs
cmake/ctest inside the vcvarsall environment. The raw preset commands below remain valid
inside such a shell.

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

With `VCPKG_ROOT` set, `dev` builds Debug and the CLI checks wiring. Package presets stay
fail-closed until runtime/licensing gates pass. Keep outputs in `out/`.

## Coding Style and Naming

Indent C++/QML four spaces, JSON/YAML two, cap C++ at 100 columns. Use attached braces,
left-bound pointers, deterministic includes, `clang-format` 19.1.5, `qmlformat`; matching
`clang-tidy` and warning-fatal `qmllint` must pass. C++ types and files use `PascalCase`,
functions and variables use `lowerCamelCase`, constants use `kPascalCase`, and namespaces use
`dvs::<module>`. QML components use `PascalCase.qml`; IDs and properties use `lowerCamelCase`.

## Testing and Performance

Use GoogleTest for C++ and Qt Quick Test for UI. Name C++ tests
`ThingTests.cpp` and QML tests `tst_thing.qml`; label every CTest case by layer and module.
`domain` and `application` suites require 80% line coverage.

On D3D11VA runners, 2-3 source 1080p60 playback must run five minutes. Exclude first two
seconds and seek/pause intervals. Never split a frame set — every display
update shows one canonical frame position across all sources; set-level frame drops
are at most 0.5%, seek P95 at most 500 ms, UI response at most 100 ms, and decoded-frame
cache usage at most 256 MiB.

## Commits, Pull Requests, and Agent Invariants

Use Conventional Commits subjects, e.g. `feat: add exact frame stepping`. PRs must explain
changes, link issues, and list tests. Add performance evidence for media/render work,
screenshots for visible QML changes, and package validation.

Never block GUI/render threads, publish partial frame sets, or omit session/generation/request
identity on asynchronous work. Hide FFmpeg/D3D11 types behind adapters. Write project, user,
and output files transactionally. Preserve every approved test and performance gate.
