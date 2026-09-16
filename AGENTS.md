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

From repository root:

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
