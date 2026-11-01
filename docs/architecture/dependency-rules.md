# Dependency rules

> **Status note.** The CMake targets below are split as shown and validated by
> `cmake/Architecture.cmake`; `dvs_platform_windows` and `dvs_ui_qml` are interface-only
> compatibility aggregates. Platform implementation files are grouped under `src/support/` and
> `src/graphics/`; UI implementation files are grouped by feature. Public include paths remain
> stable. A feature directory does not itself define a CMake target: consult the component's
> `CMakeLists.txt` and the [repository map](repository-map.md) for target ownership.

Production dependencies point inward or toward an explicit adapter contract:

```text
domain
  ↑
application        presentation_contract
  ↑                         ↑
windows_support       graphics_d3d11
  ↑                         ↑
persistence_json      ui_d3d11_bridge
media_ffmpeg          ui_models
          \            /
             app composition
```

- Domain and Application contain no Qt, FFmpeg, JSON, Win32, or D3D11 types.
- `presentation_contract` is pure C++ and is the only source of comparison enums and capability
  descriptors.
- `windows_support` and `graphics_d3d11` are separate targets. `dvs_platform_windows` is only a
  compatibility aggregate.
- `media_ffmpeg` links both precise targets because probing/cache publication uses Windows support
  while decode and frame resources use the graphics boundary.
- `ui_models` depends on Qt Core, Application, and Presentation Contract; it does not link D3D11.
- `ui_d3d11_bridge` owns Qt Quick scene-graph and renderer integration.
- The application composition root is the only place allowed to construct concrete adapters.

`cmake/Architecture.cmake` is the executable allow-list for these rules. New production targets
must be registered there instead of hiding project dependencies in generator expressions.

Build profiles enforce the same boundary at configure time:

- `core-dev` builds Domain, Application, Presentation Contract, and unit tests only. It is the
  profile used by Linux/Clang CI.
- `ui-dev` adds Qt Core UI models without FFmpeg or D3D11 adapters.
- `dev` and `release` compose the complete Windows desktop product.
