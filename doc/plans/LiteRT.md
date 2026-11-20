# LiteRT-LM Capture and Replay Plan

LiteRT-LM is installed in `/opt/litert-lm`. Target proof-of-concept test:
`tracetooltests` `vulkan_llm_pingpong`.

## Architecture & Findings

1. **C API Interposition (`LD_PRELOAD` + `dlvsym`)**:
   - `liblitert-lm.so` only exports the C API (`litert_lm_*`) and is linked with
     `-Wl,-Bsymbolic`; no C++ symbols are exported.
   - All exported symbols use ELF version `LITERT_LM_C_API_1.0`. Lavatube's
     `liblitert-lm.so` shim exports the intercepted `litert_lm_*` functions with
     a matching version script and resolves upstream symbols via
     `dlvsym(RTLD_NEXT, ..., "LITERT_LM_C_API_1.0")` (or `dlopen`).
1. **Suppressing Internal Backend Vulkan Calls**:
   - On GPU (`libwebgpu_dawn.so`), LiteRT-LM creates its own Vulkan
     instance/device under the hood.
   - A thread-local `in_litert_lm_call` guard bypasses Vulkan layer capture
     while inside any `litert_lm_*` call so only the application's Vulkan
     commands and high-level LiteRT-LM packets are recorded.
1. **Model Path Handling**:
   - Do **not** store full host filesystem paths in the trace. Store a basename,
     parent-directory hint, packaging UUID when available, file size, and
     sampled fingerprint. Replay requires a matching model identity.
   - On replay, `lava-replay` resolves the model via an explicit CLI option (or
     `LITERT_MODEL_PATH`) or searches standard model directories
     (`~/.litert-lm/models/`, `/opt/litert-lm/models/`).

## Implementation Steps

1. **Packet & Dictionary Plumbing (`scripts/litert_lm.py`)**:
   - Generate `generated/litert_lm_auto.{h,cpp}` with
     `LITERT_LM_DICTIONARY_BASE = 0x9000`.
   - Add `PACKET_LITERT_LM_API_CALL = 14` in `src/lavatube.h` and wire it
     through `src/read.cpp`, `src/hardcode_read.cpp` (`switchboard_packet`),
     `lava-print`, and `lava-cli`.
   - Track opaque handles (`LiteRtLmEngineSettings`, `LiteRtLmEngine`,
     `LiteRtLmSessionConfig`, `LiteRtLmSamplerParams`, `LiteRtLmSession`,
     `LiteRtLmInputData`, `LiteRtLmResponses`) in `limits.json` and
     `tracking.json`.
1. **Capture Layer (`src/litert_lm_layer.cpp`)**:
   - Intercept the C API calls used by `vulkan_llm_pingpong`:
     - Engine/settings: `litert_lm_engine_settings_create` (storing model
       identity), `litert_lm_engine_settings_delete`, `litert_lm_engine_create`,
       `litert_lm_engine_delete`
     - Session config/sampler: `litert_lm_session_config_create`,
       `litert_lm_session_config_set_max_output_tokens`,
       `litert_lm_session_config_set_sampler_params`,
       `litert_lm_session_config_delete`, `litert_lm_sampler_params_create`,
       `litert_lm_sampler_params_set_{top_p,top_k,temperature,seed}`,
       `litert_lm_sampler_params_delete`
     - Session & inference: `litert_lm_engine_create_session`,
       `litert_lm_session_delete`, `litert_lm_input_data_create`,
       `litert_lm_input_data_delete`, `litert_lm_session_generate_content` (and
       `run_prefill` / `run_decode`), `litert_lm_responses_delete`
     - Host getters (`litert_lm_responses_get_num_candidates`,
       `litert_lm_responses_get_response_text_at`) record the application's
       response reads. Vertex buffer updates are captured separately via Vulkan
       host-memory tracking.
   - Preload the LiteRT-LM shim in `scripts/lava-capture.py`.
1. **Replay (`lava-replay`)**:
   - Dynamically load `liblitert-lm.so` on demand when a trace contains
     LiteRT-LM calls.
   - Add CLI options to override model path and LiteRT-LM backend (`cpu`/`gpu`).
   - Execute recorded LiteRT-LM calls in `read_litert_lm_apicall()`.
1. **Testing**:
   - Add a CTest target capturing and replaying `vulkan_llm_pingpong`, verifying
     `vkAssertBuffer` checksums and LiteRT-LM packet serialization.

## Cross-platform output validation

### Scope and limitations

API replay repeats inference on captured inputs. It does not rerun application
logic that consumes the new responses. Recorded Vulkan calls therefore display
the captured application's decisions, even when replay generates different
responses. Later inference requests also contain the captured inputs, rather
than inputs recalculated from the replayed responses. The replay window cannot
prove that inference is correct.

For example, replaying ping-pong does not make the paddle follow a newly
generated decision. A validator can judge that decision against the recorded
ball and paddle state. Measuring how the bot actually plays with its new
decisions requires a live application run on the target platform.

Schema constraints restrict output structure and permitted values, but do not
establish application correctness. Exact text comparisons remain useful under
controlled, repeatable conditions; valid output may differ across platforms.

### Phase 1: inspectable model outputs

Implemented: save response reads automatically in `lavaresults.json`, under
`litert_lm.outputs`, independently of `--litert-verify-output`. Each entry
includes the captured and replayed text and API status, plus call, thread,
packet, session, response, and candidate identifiers. Copy native response text
when it is read so that later calls or rendering cannot change the saved result.
Represent unavailable results explicitly. Preserve existing performance and
verification blocks.

This first step covers the response reads recorded through supported session and
synchronous conversation APIs. Conversation outputs retain the complete response
JSON and separate conversation/JSON response identifiers. It does not fetch
additional candidates or unread responses. Vulkan-only replay does not add a
LiteRT-LM output block. See [LiteRT-LM.md](../LiteRT-LM.md) for the implemented
JSON fields and limitations.

### Phase 2: review context and readable reports

Extend the output artifact with captured inputs associated with each inference
request, model identity, runtime and backend information, generation settings,
and trace identity. Include enough identifiers to locate requests and their
preceding session history without treating every response as an independent
conversation.

Generate an HTML report from this data with captured and replayed responses side
by side, their inputs, and any verification failures. This gives users a
consistent place to inspect actual inference results, including for applications
without a window. Captured graphics must not be presented as evidence of
inference validity.

### Phase 3: application-specific validators

Provide an interface for explicitly supplied external validators to consume the
output artifact and produce machine-readable outcomes and diagnostics. Do not
automatically execute code embedded in a trace.

Validators define application correctness: acceptable paddle decisions given a
recorded game state, expected extracted values, or tests of generated code.
Allow multiple correct outputs, tolerances, and aggregate success criteria.
Generic schema validation is not a substitute for these checks. The synchronous
conversation and per-message constraint subset used by ping-pong is now captured
and replayed; additional APIs, including streaming, still need support for
workloads that use them.

Keep application validation outcomes separate from exact output comparison and
API failures. Passing one check does not imply passing the others. Validate
recorded situations without claiming that this tests an application's live
feedback loop on the target platform.

### Phase 4: reviewed reference outputs

Where an automatic correctness check is unavailable, let users explicitly accept
a reviewed replay run as a reference, stored separately from the trace.
Associate it with trace identity, model identity, runtime, backend, generation
settings, and relevant platform information. Never silently replace an accepted
reference.

Future runs can compare their output with the selected reference. Exact
agreement requires repeatable inference; otherwise an application-specific
acceptance rule is necessary. Report reference agreement separately from
application validation: an approved reference establishes an accepted result,
not independent proof of correctness. Changing a platform or runtime may require
another reviewed reference or application validation run.
