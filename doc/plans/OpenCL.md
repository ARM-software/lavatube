# OpenCL support

## Goals

- The purpose is mostly experimentation for now. OpenCL has many challenges, especially
  in regards to portability, that we probably need to figure out as we go.
- We want to support OpenCL and Vulkan inter-operability, and save their calls to one
  trace container with thread synchronization and resource sharing working between the
  two.
- We have no users so we can afford to break trace compatibility later if we make poor
  decisions now.

## Done

- tracetooltests and chameleon support OpenCL already.
- `external/tracetooltests/scripts/opencl_spec.py` as basis for code generation

## Preparation

- What code reorganization and refactoring should we do first?
- Do we need a shared-runtime refactor? The Vulkan layer compiles the capture
  implementation directly into its DSO, including its static writer singleton.
  Repeating that for an OpenCL layer would create two writers?

## Capture

- We want to store both APIs in the same trace container.
- Use OpenCL loader layers as the primary capture mechanism for now. Layer support is
  still experimental and loader-dependent. High uncertainty for Android which we can
  figure out later.
- We should store OpenCL calls intermixed with Vulkan calls if they come from the same thread.
- Both Vulkan and OpenCL should be served from the same DLL so they share one singleton.
- clCreateContext must initialize capture the same way that vkCreateInstance does (if not
  already initialized)

## Replay

- We want to replay both APIs with a single replayer binary.

## Multi-threading

- We want to re-use the same thread-model as we use with Vulkan now, but most OpenCL calls
  are explicitly thread-safe.

## Documentation

- https://github.com/Kerilk/OpenCL-Layers-Tutorial
- https://github.com/KhronosGroup/OpenCL-Layers
