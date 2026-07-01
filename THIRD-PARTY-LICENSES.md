# Third-Party Licenses

Lattice is licensed under the MIT License (see [`LICENSE`](LICENSE)).
It builds on a number of third-party libraries that are **fetched at configure
time** by CMake (`FetchContent` / a single-header download) rather than vendored
in this repository. Each retains its own license, reproduced or linked below.

A game shipped on this engine links several of these libraries, so their license
terms — chiefly the requirement to preserve copyright/permission notices — apply
to the distributed binary. This file is the attribution starting point; verify it
against the exact versions you ship.

| Library | Version | License | Used for |
|---|---|---|---|
| [bgfx / bimg / bx](https://github.com/bkaradzic/bgfx) (via [bgfx.cmake](https://github.com/bkaradzic/bgfx.cmake)) | v1.143.9257-544 | BSD 2-Clause | Rendering backend, image decode, base library |
| [GLM](https://github.com/g-truc/glm) | 1.0.1 | MIT (The Happy Bunny / MIT dual) | Math types (`WorldCoord`) |
| [yaml-cpp](https://github.com/jbeder/yaml-cpp) | 0.8.0 | MIT | Layer-config YAML parsing |
| [GLFW](https://github.com/glfw/glfw) | 3.4 | zlib/libpng | Windowing and input |
| [ENet](https://github.com/lsalzman/enet) | v1.3.17 | MIT | UDP networking transport (M11) |
| [miniaudio](https://github.com/mackron/miniaudio) | 0.11.21 | Public domain (Unlicense) or MIT-0 | Audio playback (M12) |
| [GoogleTest](https://github.com/google/googletest) | (pinned to `main`) | BSD 3-Clause | Unit-test framework (build/test only — not shipped) |

## Notes

- **GoogleTest** is a build- and test-time dependency only; it is not linked into
  the engine library or any game binary, so its license does not propagate to a
  shipped game.
- **bgfx, ENet, and miniaudio** are used strictly as engine internals — none of
  their types appear in the public API under `include/` (ARCHITECTURE §12/§16) —
  but they are statically linked into the engine, so their notices must travel
  with a shipped binary.
- Versions above mirror the pins in the root [`CMakeLists.txt`](CMakeLists.txt).
  When you bump a dependency there, update this file to match.
- Full license texts are retrieved with each dependency's source under your build
  directory (`build/_deps/<name>-src/`); consult those for the authoritative,
  version-exact terms.
