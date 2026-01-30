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
- Do not modify generated files in `generated/` directly. First line in a generated file says which script generated it.
- If there is an error in the app that we capture, we should be resistant to this and keep running if possible; but
  if there are problems caused by our capturing code, we should fail as early as possible with a clear error message so we
  can fix them, not try to work around them with defensive code.

## Testing Guidelines
- Add new tests under `tests/` (see `container_test.cpp`, `tracing*.cpp` for patterns) and reuse helpers in `tests/common.*`.
- Favor GPU-independent checks; prefer noscreen/blackhole modes when possible to keep runs fast and deterministic.
- Run `ctest` (or `make test`) before submitting; include any trace assets needed for reproducing issues but avoid committing large binaries.

## Coding
- Keep in mind that lavatube is multi-threaded. For more information on the multi-threaded design, see [Multithreading.md](doc/Multithreading.md).
- For investigations into memory management, read [MemoryManagement.md](doc/MemoryManagement.md).

## Debugging
- For troubleshooting, set `LAVATUBE_DEBUG` (value from 1 to 3) but keep default output quiet in commits.

## Modifying capture functionality
- Capture is often also called `write` or `trace` in the code.
- Capture file IO code is in `src/filewriter.cpp` and higher-level code in `src/write.cpp`.
- Manually implemented functions are found in `src/hardcode_write.cpp`
- Capture is often initiated from the script `scripts/lava-capture.py`

## Modifying replay functionality (lava-replay)
- Replay is often also called `read` or `retrace` in the code.
- Replay file IO code is in `src/filereader.cpp` and higher-level code in `src/read.cpp`.
- Manually implemented functions are found in `src/hardcode_read.cpp`. Replacement functions are called `retrace_<name>`.
- Callbacks called before the function are called `replay_pre_<name>` and callbacks called after are called `replay_post_<name>`.
- The memory suballocator is in `src/suballocator.cpp`
- General window management (WSI) code is in `src/window.cpp`
- The replay binary is built from `src/replay.cpp`

## Modifying post-processing functionality (lava-tool)
- The post-processing tool uses both capture and replay functionality (as described above).
- The post-process binary is built from `src/tool.cpp` and most of its code is in callbacks called `postprocess_<name>`.
  The `CALLBACK` macro is used to register callbacks.
- Most post-process callback functions are in `src/postprocess.cpp`, but if they require tool context, they are in `src/tool`.
  Some special callbacks eg for draw calls have hardcoded calls created in `scripts/util.py`.
- SPIRV simulation is handled in `src/execute_commands.cpp`
