# LiteRT-LM

Lavatube has experimental support for Google's LiteRT-LM API, specifically
its C API and any other API that in turn uses the C API. It captures the
tokenized input to the prefill stage, which means on replay we exercise
both prefill and decode stages. You need to use the same model for both
capture and replay.

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
