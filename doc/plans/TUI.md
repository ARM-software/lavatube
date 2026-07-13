# TUI

## What it is

New tool `lava-tui` that communicates with `lava-replay` (or `lava-tool`) similarly
to `lava-cli` but implements a text-based user-interface that calls LLM models to
implement a natural language interface similar to codex.

It offers a text user-interface for manipulating the currently running replay service,
backed by an LLM engine that translates free text into actions and investigations. It
is meant to be run from a desktop or laptop host, while the replayer could run locally
or on a devboard or Android device.

## TODO

Remaining todo list:
* Add support for `lava-tui <trace file>` that launches the replayer
	- Either launch with gdb
	- Or set core file vars and use c++ callstack ops to fetch callstacks whenever (maybe try this in lava-cli first)
	- This would only work locally, although we could support replaying over adb on Android.
* Remove the TUI's own commands.
	- Or always launch it with local copy of trace file so that we can inspect it simultaneously while replaying it.
	- We can have need for being able to run the equivalent of `lava-print` commands on the trace
* Multi-model support (see below)

## Implementation

We interface directly with models using `libcurl` and the OpenAI response JSON format.

We need to efficiently describe the actions that the agent can take and respond with, and
even more efficiently trim down any information provided to avoid overwhelming the agent
with tokens and context.

All actions currently supported by `lava-replay` over its CLI TCP connection interface should
be supported. We also support some actions directly on a trace file that also work even if
we don't connect to a replayer.

## Visual looks

The screen is composed into three sections: The view, the input line and the status line.
At the very bottom we have the status line with information such the current model. Just
above the status line we have input field where the user can enter prompt text. The remaining
screen space is taken up by the transcript view which shows all the model output.

## Separation of concerns

`lava-tui` is meant as a human interface for interfacing with a trace and trace replay, while
`lava-cli` is an agent interface to the same. The `lava-tui` interface _could_ be entirely
replaced by a user using `codex` (or similar) tool, where the real agent CLI would be far
more powerful. The advantages of our TUI would come from closer integration, faster iteration,
closer prompt control, and local model support. It is not yet entirely clear what are the real
advantages of one way over the other.

## Multi-model support

We would like to run most commands through a locally-hosted LLM for lower latency and saving
token costs. We will implement this using a router-first design similar to how `Gemini CLI`
implemented it - a local model is used to classify the request and we use this classification
to pick either local or cloud model to provide an answer. This way we retain the integrity
of the transcript.

The routing question should provide a JSON response that gives `routing` recommendation (`local`
vs `cloud`), floating-point `confidence`, and a `reason` very brief free text. Then `lava-tui`
can use this to decide where to send the request using a confidence threshold that overrides to
cloud model by default for now (until we get more confidence with the local model). We should
print this as metadata (ie not part of chat history) in the transcript view which model did
respond along with the confidence and reason. The router should also check if the user
explicitly requests either a local or cloud response.

New user settings (with defaults):
```
LAVATUI_LOCAL_BASE_URL=http://localhost:11434/v1
LAVATUI_LOCAL_MODEL=gemma4:latest
LAVATUI_CLOUD_BASE_URL=https://api.openai.com/v1
LAVATUI_CLOUD_MODEL=gpt-5.5
LAVATUI_CLOUD_REASONING=low
LAVATUI_LLM_MODE=routed
```

New user settings with no defaults:
```
LAVATUI_LOCAL_API_KEY=...
LAVATUI_CLOUD_API_KEY=...
```

If `LAVATUI_LLM_MODE` is set to `routed` we route between local and cloud models. Otherwise
it is set to either `local` or `cloud`. If mode is not set, but we have settings for both models,
we assume `routed`. If we only have settings for one model, we use that one. The user should be
able to switch on-the-fly between the three settings by typing `/local`, `/cloud` or `/routed`.

`LAVATUI_LOCAL_API_KEY` must be set to `ollama` for local models for now.

Since this is still just an experiment, we do not need to care about backwards compatibility
for users. We want to abort on errors with clear error messages so we can fix them rather than
try to work around them.

### Architecture

```
tui_component -> tui_llm_router -> tui_llm_client
                                -> local responses client
                                -> cloud responses client
```

### Implementation steps

A first step could be to change the environment variables and implement an on-the-fly user switch
between cloud and local.

Then as a later second step we can implement the routing option.
