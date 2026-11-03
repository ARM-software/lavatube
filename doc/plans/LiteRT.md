# LiteRT-LM Capture and Replay Plan

LiteRT-LM is installed in `/opt/litert-lm` (built from `/home/per/LiteRT-LM`). Target proof-of-concept test:
`tracetooltests` `vulkan_llm_pingpong`.

## Architecture & Findings

1. **C API Interposition (`LD_PRELOAD` + `dlvsym`)**:
   - `liblitert-lm.so` only exports the C API (`litert_lm_*`) and is linked with `-Wl,-Bsymbolic`; no C++ symbols are
     exported.
   - All exported symbols use ELF version `LITERT_LM_C_API_1.0`. `libVkLayer_lavatube.so` will export the intercepted
     `litert_lm_*` functions with a matching version script and resolve upstream symbols via
     `dlvsym(RTLD_NEXT, ..., "LITERT_LM_C_API_1.0")` (or `dlopen`).
1. **Suppressing Internal Backend Vulkan Calls**:
   - On GPU (`libwebgpu_dawn.so`), LiteRT-LM creates its own Vulkan instance/device under the hood.
   - A thread-local `in_litert_lm_call` guard in `libVkLayer_lavatube.so` will bypass Vulkan layer capture while inside
     any `litert_lm_*` call so only the application's Vulkan commands and high-level LiteRT-LM packets are recorded.
1. **Model Path Handling**:
   - Do **not** store full host filesystem paths in the trace; store only the model basename (e.g.
     `gemma3-1b-it-int4.litertlm`).
   - On replay, `lava-replay` resolves the model via an explicit CLI option (or `LITERT_MODEL_PATH`) or searches
     standard model directories (`~/.litert-lm/models/`, `/opt/litert-lm/models/`).

## Implementation Steps

1. **Packet & Dictionary Plumbing (`scripts/litert_lm.py`)**:
   - Generate `generated/litert_lm_auto.{h,cpp}` with `LITERT_LM_DICTIONARY_BASE = 0x9000`.
   - Add `PACKET_LITERT_LM_API_CALL = 14` in `src/lavatube.h` and wire it through `src/read.cpp`,
     `src/hardcode_read.cpp` (`switchboard_packet`), `lava-print`, and `lava-cli`.
   - Track opaque handles (`LiteRtLmEngineSettings`, `LiteRtLmEngine`, `LiteRtLmSessionConfig`, `LiteRtLmSamplerParams`,
     `LiteRtLmSession`, `LiteRtLmInputData`, `LiteRtLmResponses`) in `limits.json` and `tracking.json`.
1. **Capture Layer (`src/litert_lm_layer.cpp`)**:
   - Intercept the C API calls used by `vulkan_llm_pingpong`:
     - Engine/settings: `litert_lm_engine_settings_create` (storing basename only), `litert_lm_engine_settings_delete`,
       `litert_lm_engine_create`, `litert_lm_engine_delete`
     - Session config/sampler: `litert_lm_session_config_create`, `litert_lm_session_config_set_max_output_tokens`,
       `litert_lm_session_config_set_sampler_params`, `litert_lm_session_config_delete`,
       `litert_lm_sampler_params_create`, `litert_lm_sampler_params_set_{top_p,top_k,temperature,seed}`,
       `litert_lm_sampler_params_delete`
     - Session & inference: `litert_lm_engine_create_session`, `litert_lm_session_delete`,
       `litert_lm_input_data_create`, `litert_lm_input_data_delete`, `litert_lm_session_generate_content` (and
       `run_prefill` / `run_decode`), `litert_lm_responses_delete`
     - Host getters (`litert_lm_responses_get_num_candidates`, `litert_lm_responses_get_response_text_at`) pass through
       on capture; vertex buffer updates are already captured via Vulkan host-memory tracking.
   - Preload `libVkLayer_lavatube.so` in `scripts/lava-capture.py`.
1. **Replay (`lava-replay`)**:
   - Dynamically load `liblitert-lm.so` on demand when a trace contains LiteRT-LM calls.
   - Add CLI options to override model path and LiteRT-LM backend (`cpu`/`gpu`).
   - Execute recorded LiteRT-LM calls in `read_litert_lm_apicall()`.
1. **Testing**:
   - Add a CTest target capturing and replaying `vulkan_llm_pingpong`, verifying `vkAssertBuffer` checksums and
     LiteRT-LM packet serialization.
