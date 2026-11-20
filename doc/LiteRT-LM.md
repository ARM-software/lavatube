# LiteRT-LM

Lavatube has experimental capture and replay support for selected LiteRT-LM C
session and synchronous conversation APIs, including wrappers that invoke those
intercepted functions. It captures input data, conversation messages, generation
calls, and response reads. For text generation, replay repeats tokenization,
prefill, and decode using LiteRT-LM. Capture and replay require the same model.
Conversation configuration, session settings, LLGuidance provider selection,
per-message regex/JSON Schema constraints, and synchronous JSON responses are
supported. Streaming and embedding APIs are not captured; other configuration
setters outside the supported subset remain passthrough calls.

## Replay statistics

Replay automatically enables LiteRT-LM benchmarking and prints session and
conversation metrics when replay finishes: prefill and decode speeds, token
counts, engine initialization time, and time to first token. Multiple prefill or
decode turns are reported separately. Time to first token excludes engine
initialization. These are runtime measurements of the replayed workload.
Vulkan-only traces produce no LiteRT-LM statistics.

Available metrics are also saved in `lavaresults.json` under
`litert_lm.sessions`. Each session has its index, `init_time_seconds`,
`time_to_first_token_seconds`, and separate `prefill` and `decode` arrays
containing `tokens` and `tokens_per_second` for each measurement. Sessions with
unavailable metrics are omitted. Ordinary Vulkan-only replays have no
`litert_lm` section.

Conversation metrics use the same fields under `litert_lm.conversations`, with
`conversation_index` in place of `session_index`.

## Model outputs

Replay automatically saves recorded response reads in `lavaresults.json` under
`litert_lm.outputs`, including when exact output verification is disabled. Each
entry contains:

- `call`, `thread`, and `packet`: the response getter's trace location.
- `session_index`, `responses_index`, and `candidate_index`: the response's
  identifiers for session responses. An unknown session is `null`.
- Conversation JSON reads instead have `conversation_index`,
  `json_response_index`, and `format: "conversation_json"`. Their `text` is the
  complete JSON response returned by LiteRT-LM, including its content blocks.
- `output_requested`: whether the captured caller supplied an output pointer.
- `captured` and `replayed`: objects containing the getter's `status` and
  `text`. Status zero means success. A replay status of `null` means the getter
  was not executed. Text is `null` when unavailable; an empty string is an empty
  result.

For example, an entry can contain `"captured": {"status": 0, "text": "UP"}` and
`"replayed": {"status": 0, "text": "DOWN"}`. Text is copied when read and
remains available after the native response is deleted. The current trace format
does not distinguish a captured successful null session text pointer from an
empty string. Entries follow replay execution order; use the trace identifiers
to match runs rather than relying on array positions across threads.

Only response reads recorded through the supported session and synchronous
conversation APIs are included. Repeated reads produce separate entries; unread
responses and additional replay candidates are not collected. A LiteRT-LM replay
with no response reads has an empty `outputs` array. Vulkan-only and blackhole
replays add no output block.

Replay does not execute the application's CPU logic that consumes model output.
Recorded Vulkan calls can therefore display the captured result even when replay
generates different text. Inspect `outputs` to review inference results
directly; the replay window does not validate the new responses. Later inference
inputs also remain the captured inputs. See the
[validation plan](plans/LiteRT.md#cross-platform-output-validation) for planned
review reports, application validators, and reviewed reference outputs.

## Output verification

Use `lava-replay --litert-verify-output trace.api` to compare replay with the
captured LiteRT-LM response text, candidate counts, and API return statuses.
Verification preserves the captured sampler, temperature, seed, and inputs. For
exact comparisons, use the same model, runtime, and backend under controlled
conditions. A fixed seed alone does not guarantee identical output across
hardware or runtime versions.

Capture stores response text and candidate counts when the application reads
them through the intercepted session response APIs or the conversation JSON
response getter. Verification checks those recorded reads; it does not collect
output the application never requested. Text comparisons are byte-for-byte and
report the first differing byte, with the call, session or conversation,
response, and candidate identifiers where available.

Replay finishes and writes `lavaresults.json` even if verification finds
mismatches. The `litert_lm.verification` object contains `status` (`passed`,
`failed`, or `no_output`), `statuses_checked`, `candidate_counts_checked`,
`responses_checked`, `unavailable_checks`, and a `mismatches` array with failure
details. Replay returns a nonzero exit status for a mismatch, unavailable
checks, or no response text to verify. Without the option, replay does not
compare output or add verification results. The option cannot be combined with
`--blackhole`.

The minimal LiteRT-LM CTest replay enables verification alongside performance
checks. Its capture defaults to temperature zero and a fixed seed.

## Ping-pong integer movement test

The ping-pong capture runs 60 frames so that it includes both the initial
palette text query and a movement query when the ball rebounds from the right
wall. Movement uses a separate synchronous conversation with LLGuidance and this
per-message JSON Schema:

```json
{"type": "integer", "minimum": -100, "maximum": 100}
```

The integer is a target paddle position: -100 is the topmost legal center, zero
is the center, and 100 is the bottommost legal center. The paddle moves toward
that position at a fixed speed and stops when it arrives. The movement response
does not alter ball physics or colors. The prompt supplies ball position,
velocity, reflection limits, and estimated time until it reaches the paddle.

Replay CTests require both palette and movement outputs, check that captured and
replayed movement responses are integers in range, and require conversation
metrics. They do not require identical target positions across backends or claim
that a target intercepts the ball correctly.

## LiteRT-LM model identification

Capture records the model filename, its parent directory as a readable model ID,
the embedded packaging UUID when present, file size, and a sampled fingerprint.
These appear in the engine-settings packet, printed calls, and `tracking.json`.
The model itself is not included in the trace.

The fingerprint is named `sampled-fnv1a64-v1`. It hashes the 32-byte file
prefix, the complete FlatBuffer header, and three samples of up to 64 KiB from
the start, middle, and end of the payload, in that order. Headers larger than 16
MiB are rejected. This identifies ordinary model copies without reading all
weights. It is not a whole-file integrity checksum: changes outside the sampled
regions can go undetected. The UUID identifies a packaged artifact and may be
absent in older models.

Replay accepts a candidate only when its size, sampled fingerprint, and recorded
UUID (if present) match. Directory names are search hints and need not match.

Use `--litert-model-path` or `LITERT_MODEL_PATH` to select a file or directory.
An explicit file must match; an explicit directory is searched for a matching
model. An invalid override fails rather than falling back to another location.

Without an override, replay searches:

1. The current directory.
1. `$LITERT_LM_DIR/models`, if set, otherwise `~/.litert-lm/models`.
1. `/opt/litert-lm/models`.

The model stores are searched at their root and one directory level below it,
matching LiteRT-LM's `<model-id>/model.litertlm` layout. Replay first tries the
recorded directory and filename, then other `.litertlm` files, allowing renamed
copies to be found. Cache files are excluded.
