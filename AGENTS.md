# Repository Guidelines

## Project Structure & Module Organization
- `src/`: core tracer, replay, Vulkan layer code; headers live alongside implementations.
- `tests/`: C++ regression and performance tests plus small generators (`gen_*`). Extend here when adding features.
- `cmake/`: toolchain presets for platform cross-compilation
- `scripts/`: Python code generators (e.g., `lava.py` generation). 
- `doc/` holds design notes
- `external/` tracks vendored deps
- `traces/` can store sample trace files
- `generated/` holds generated c++ files created by the python scripts
- The loader manifest is `VkLayer_lavatube.json`.
- Use separate build directories (e.g. `build/`) to keep the tree clean.
- Files and classes with `write` in the name are related to trace capture, while `read` is related to trace replay.

## Build, Test, and Development Commands
- Bootstrap deps: `git submodule update --init --recursive`.
- Configure: `mkdir -p build && cd build && cmake ..` (add `-DCMAKE_BUILD_TYPE=Debug` while iterating).
- Build: `make -C build -j$(nproc)`.
- Tests: `cd build && make test` or `ctest -V` for verbose runs.

## Coding Style & Naming Conventions
- C++ with tab indentation and Allman braces (opening brace on a new line). Mirror existing spacing and logging patterns.
- Names are generally lower_snake_case for functions/files; macros and constants stay uppercase.
- Keep headers and implementations paired in `src/`; avoid introducing extra dependencies without CMake updates.

## Testing Guidelines
- Add new tests under `tests/` (see `container_test.cpp`, `tracing*.cpp` for patterns) and reuse helpers in `tests/common.*`.
- Favor GPU-independent checks; prefer noscreen/blackhole modes when possible to keep runs fast and deterministic.
- Run `ctest` (or `make test`) before submitting; include any trace assets needed for reproducing issues but avoid committing large binaries.

## Commit & Pull Request Guidelines
- Use short, imperative commit subjects similar to existing history (“Add better logging for address translations”, “Handle vkWaitSemaphores replay properly”); keep the first line ≤72 chars.
- PRs should summarize scope, risks, and reproduction steps; link issues when applicable.
- Always note test coverage (commands and results)

## Debugging
- For troubleshooting, set `LAVATUBE_DEBUG` (1–3) but keep default output quiet in commits.
