# TUI

Remaining todo list:
* Implement `lava-tui` that connects to the current replay service, just like
  `lava-cli` currently does, and gets its tool commands from it instead. And
  remove the TUI's own commands.
* Finally, add back support for `lava-tui <trace file>` again.

New tool `lava-tui` that communicates with `lava-replay` (or `lava-tool`) similarly
to `lava-cli`.

Started as `lava-tui`, then offers a text user-interface for manipulating the currently
running replay service, backed by an LLM engine that translates free text into actions
and investigations.

We will also offer `lava-tui <trace file>` as before - in which case we could run it
from inside `lava-tui` but with gdb (or some other way to get a call stack on crash),
so crashes can be provided to the LLM if they happen.

We should skip building this for Android to avoid it being burdened by any new dependencies.

We interface directly with chatgpt using `libcurl`. This uses a JSON format.

We need to efficiently describe the actions that the agent can take and respond with, and
even more efficiently trim down any information provided to avoid overwhelming the agent
with tokens and context.

## Visual looks

The screen should be composed into three sections: The view, the input line and the status line.

At the very bottom we have the status line with information such the current model.

Just above the status line we have input field where the user can enter prompt text. If the
text exceeds one line, it should ideally expand in size, but if that is difficult it could also
start multi-line and support scrolling instead.

The remaining screen space is taken up by the view which shows all the model output.

## Actions

All actions currently supported by `lava-replay` over its TCP connection interface should be supported.

We currently implement these directly:
- get list of objects created (from `limits.json`, prune all zero value entries, then turned into TSV using datatable)
- get frame meta info (thread X frame Y from `frames_<X>.json` selecting frame Y in `frames` key, JSON format)
- get thread X meta info (all entries from `frames_<X>.json` except for `frames` key, JSON format)
- get meta info for object type X index Y (from `tracking.json`, format JSON)

## Long-term plan

We would like to run most commands through a locally-hosted LLM for low-latency and saving
token costs, but have it evaluate the complexity of the prompt then pass high-complexity
prompts to chatgpt. We would use whichever tool allows us to reuse our chatgpt implementation,
probably ollama or vllm. Our Intel GPU-based laptops have very poor support for GPU accelerated
models, so best to dedicate some desktop with a more powerful GPU as a shared server for this.

## Separation of concerns

`lava-tui` is meant as a human interface for interfacing with a trace and trace replay, while
`lava-cli` is an agent interface to the same. The `lava-tui` interface _could_ be entirely
replaced by a user using `codex` (or similar) tool, where the real agent CLI would be far
more powerful. The advantages of our TUI would come from closer integration, faster iteration,
and local model support.
