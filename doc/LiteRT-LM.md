# LiteRT-LM

Lavatube has experimental capture and replay support for selected LiteRT-LM C
session APIs, including wrappers that invoke those intercepted functions. It
captures input data and generation calls. For text generation, replay repeats
tokenization, prefill, and decode using LiteRT-LM. Capture and replay require
the same model. Conversation and embedding APIs are not currently captured.

## Replay statistics

Replay automatically enables LiteRT-LM benchmarking and prints session metrics
when replay finishes: prefill and decode speeds, token counts, engine
initialization time, and time to first token. Multiple prefill or decode turns
are reported separately. Time to first token excludes engine initialization.
These are runtime measurements of the replayed workload. Vulkan-only traces
produce no LiteRT-LM statistics.

Available metrics are also saved in `lavaresults.json` under
`litert_lm.sessions`. Each session has its index, `init_time_seconds`,
`time_to_first_token_seconds`, and separate `prefill` and `decode` arrays
containing `tokens` and `tokens_per_second` for each measurement. Sessions with
unavailable metrics are omitted, and the `litert_lm` section is absent when no
metrics are available.

## Output verification

Use `lava-replay --litert-verify-output trace.api` to compare replay with the
captured LiteRT-LM response text, candidate counts, and API return statuses.
Verification preserves the captured sampler, temperature, seed, and inputs.
For exact comparisons, use the same model, runtime, and backend under controlled
conditions. A fixed seed alone does not guarantee identical output across
hardware or runtime versions.

Capture stores response text and candidate counts when the application reads
them through the intercepted session response APIs. Verification checks those
recorded reads; it does not collect output the application never requested.
Text comparisons are byte-for-byte and report the first differing byte, with
the call, session, response, and candidate identifiers where available.

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
2. `$LITERT_LM_DIR/models`, if set, otherwise `~/.litert-lm/models`.
3. `/opt/litert-lm/models`.

The model stores are searched at their root and one directory level below it,
matching LiteRT-LM's `<model-id>/model.litertlm` layout. Replay first tries the
recorded directory and filename, then other `.litertlm` files, allowing renamed
copies to be found. Cache files are excluded.
