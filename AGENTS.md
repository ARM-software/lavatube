# Lavatube repository guidance

## Code invariants

- Use Vulkan headers in `external/tracetooltests/external/Vulkan-Headers/include/vulkan/`, not system headers.
- Edit generators, not `generated/` files; their first line identifies the generator.
- Use tabs and Allman braces for C++; follow surrounding naming and logging conventions. Do not introduce lambdas,
  anonymous namespaces, or new exception throws.
- Do not add globals. Store state carried between commands in tracked metadata in `src/lavatube.h`; register new
  `trackable` subclasses in `trackable_type_map_general` in `scripts/vkconfig.py`.
- Tolerate captured application errors when possible. Fail early with a clear diagnostic for defects in our code; do
  not hide them with workarounds or overly defensive programming.
- Keep thread state consistent throughout each packet. Prefer handing information to the owning thread for processing
  before its next packet over modifying its state directly or adding mutexes. See
  [Multithreading.md](doc/Multithreading.md) for threading changes and [MemoryManagement.md](doc/MemoryManagement.md)
  for memory work.
- File IO handles chunks; command processing handles packets. A chunk may contain multiple packets. Command IDs are
  remapped through each trace's dictionary.

## Implementation entry points

`write`/`trace` means capture; `read`/`retrace` means replay.

* Capture - `src/filewriter.cpp`, `src/write.cpp`, `src/hardcode_write.cpp`; launch via `scripts/lava-capture.py`
* Replay - `src/filereader.cpp`, `src/read.cpp`, `src/hardcode_read.cpp`, `src/replay.cpp`
* Memory allocation / WSI - `src/suballocator.cpp` / `src/window.cpp`
* Post-processing - `src/tool.cpp`, `src/postprocess.cpp`, `src/tool/`; callbacks named `postprocess_<name>`
  registered with `CALLBACK`; draw-call generation in `scripts/util.py`
* SPIR-V simulation - `src/execute_commands.cpp`, `external/spirv-simulator/`

Capture metadata lookup: `writer.parent->records.<type>_index.at(<native handle>)`. Replay and post-processing lookup:
`<type>_index.at(<index>)`. Manual replay replacements use `retrace_<name>`; pre/post callbacks use `replay_pre_<name>`
and `replay_post_<name>`.

## Builds and validation

- Use separate CMake build directories, such as `build/` and `build_release/`. Build setup is in [README.md](README.md).
- Tests live in `tests/`; reuse `tests/common.*` and existing tracing/container patterns. The full CTest suite takes
  over 15 minutes; choose checks appropriate to the change.
- Do not compile or run tests as part of code reviews.

## Trace diagnostics

- `LAVATUBE_DEBUG=1` through `3` increases logging.
- Traces are containers. For example, `build/packtool print limits.json build/vulkan_general.vk` prints object counts.
  `tracking.json` holds immutable/accumulated object metadata; `metadata.json` holds presented and requested
  capabilities.
- `build/lava-print-fast` prints calls as NDJSON. `lava-print --add-checksums` hashes logical captured data before
  replay address translation.
- `build/lava-replay --service` starts replay paused; use `build/lava-cli` to step, jump, or inspect state. Threads are
  stably paused after each CLI command. For hangs, use `lava-cli diagnose deadlock` and `lava-cli info threads`.
- `lava-replay --validate` can expose errors, though real content often produces harmless validation warnings.
- `lava-tool -S <input trace> <output trace>` simulates shaders to investigate buffer device addresses, shader binding
  tables, and descriptors. Prefer a release build; simulation can be slow.
