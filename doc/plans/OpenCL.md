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

What code reorganization and refactoring should we do first?

## Capture

- We want to store both APIs in the same trace container.
- OpenCL has layers, but pretty much nobody has implemented support for them. We will likely
  have to rely on old-fashioned dynamic linker shenanigans instead.
- Should we store OpenCL calls intermixed with Vulkan calls if they come from the same thread?

## Replay

- We want to replay both APIs with a single replayer binary.

## Multi-threading

- We want to re-use the same thread-model as we use with Vulkan now, but not sure what
  OpenCL's thread model is. TBD - figure this out.
