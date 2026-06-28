# Contributing

Thanks for your interest in the voxel engine. This guide covers how to build,
test, and submit changes. It is deliberately short; the load-bearing document is
[`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md).

## Read this first

**Before writing any non-trivial code, read [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md).**
It contains the subsystem dependency map, the hard invariants, the list of common
mistakes, and a heuristic for when to proceed independently versus when to raise a
design question. The engine enforces several constraints that are easy to violate
in ways that look correct but are subtly wrong — float promotion in world-space
paths, non-determinism in the decomposition pipeline, level-skipping in the
decomposition chain. The architecture document names them explicitly.

This applies to both human and AI contributors. If you are using an AI coding
agent, point it at the architecture document before it starts.

## Build and test

See [`README.md` → Setup](README.md#setup) for prerequisites (compiler, CMake,
and per-OS dev packages) and the full dependency list.

```bash
cmake -B build
cmake --build build
ctest --test-dir build
```

All tests must pass before a change is submitted. New behavior needs new tests —
the suites under `tests/` are the regression net, and several of them pin
byte-for-byte output (geometry, save format) precisely so that "harmless"
refactors that change results are caught.

## How the codebase is organized

- **`include/`** is the public API and propagates to consumers. Keep it minimal;
  third-party types (bgfx, ENet, miniaudio, …) must not appear here.
- **`src/`** holds engine internals, private to the library's own compilation.
- **`plugins/`** are runtime-loadable modules that link **zero** engine symbols —
  they see only `include/` plus the in-tree `Voxel` definition, and receive a
  function-pointer table. Adding `plugins/<name>/plugin.cpp` is enough; the build
  discovers it automatically.
- **`demos/`** are a progressive reference series; dropping in
  `demos/<NN-name>/main.cpp` adds a target with no CMake edits.
- **`templates/`** are copy-paste starting points, not built targets.
- **`docs/tutorials/`** are step-by-step walkthroughs.

## Coding standards

- **C++20.** First-party targets build under `-Wall -Wextra -Wpedantic` (GCC/Clang)
  or `/W4` (MSVC). Keep new code warning-clean.
- **Never use raw `double`/`float` for world-space positions** — use `WorldCoord`.
  This is a compile-time-enforced invariant; do not work around it.
- **Decomposition and generation must be deterministic** — pure functions of
  `(position, seed)`. No clocks, no global RNG, no I/O in those paths.
- Match the style, naming, and comment density of the surrounding code.

## Submitting changes

1. Branch from `main`.
2. Make the change, with tests, keeping the build warning-clean and `ctest` green.
3. Open a pull request describing **what** changed and **why**, and note any
   architecture-document sections your change touches (update them if so).
4. Keep PRs focused — one logical change per PR is much easier to review.

## Reporting issues

When filing a bug, include your OS and compiler, the CMake and build output if it
is a build failure, and the smallest set of steps that reproduces the problem.
