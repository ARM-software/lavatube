# lava-agent

New agentic experiment based on experiences from `lava-cli` and `lava-tui`.

Where `lava-cli` became a useful but primitive diagnostic tool, `lava-tui`
became unused because it was not as powerful as existing agent harnesses.
The useful parts of `lava-tui` - its close tool integration, prompt control
and local model support - can perhaps be utilized better in a different
type of tool.

`lava-agent` is meant as a subagent that operates with a limited number of
steps on a single query and reports back its findings in a structured format.
It is meant to be invoked by an external agent harness, called the controller
in this document, which will typically use a cloud model. `lava-agent` uses a
local model and runs on the same device as the replay service.

It requires:
* Local access to the trace file
* Access to an explicitly selected, already-running `lava-replay` service
  instance that it can query. The caller owns the service lifecycle and replay
  position.
* Access to a local AI model supporting an OpenAI compatible interface (like
  `lava-tui`)
* A fixed system prompt that tells it what it can and must (not) do
* A provided user prompt that tells it what to investigate
* Check with `lava-cli info trace` that the local trace path and the running
  replay refer to the same filesystem object. A lightweight device and inode
  comparison is sufficient; this is an accidental-mismatch check, not a
  security boundary, and does not require hashing the trace.
* Ability to look directly into a trace file similarly to `lava-print-fast`

It will give you:
* Structured JSON output

It must not:
* Modify a running `lava-replay` service instance. The list of tool commands
  must not include options to do so.
* Launch, load a trace into, restart, stop, or advance a `lava-replay` service
  instance.
* Require any state or history. Any context it needs should be provided by the
  controller.

We do not want:
- Persistent history
- Replay control
- Arbitrary commands
- Trace modification
- Human chat UI
- Model routing

## Interface

The simplest request would be something like:

`lava-agent --service localhost:11901 example.api ask "Is thread 3 blocked on work from another replay thread?"`

This would connect to the explicitly selected replay service and read
`example.api` locally to answer the posed question. It would not launch the
service, load the trace into it, or move its replay position.

The prompt implies that there is a running replay service that we can look at,
and we should check the trace status then investigate the trace file to find if
the suggested state is plausible. Then report whatever evidence turned up.

There could be the following options:

```
lava-agent 0.0.2-debug command line options
lava-agent --service HOST:PORT [options] <trace file> ask <prompt>
-h/--help              This help
-v/--verbose           Verbose output (if debug file enabled)
-d/--debug level       Set debug level [0,1,2,3] (if debug file enabled)
-df/--debugfile FILE   Output debug output to the given file
--service HOST:PORT     Required replay service endpoint
--timeout SECONDS       Maximum wall-clock duration (default 300)
--max-output-bytes N    Maximum result size (default 32768)
```

It should return structured evidence along the lines of:

```json
  {
    "schema_version": 1,
    "status": "answered",
    "conclusion": "...",
    "confidence": 0.8,
    "evidence": [
      {
          "tool_id": <number>,
          "tool_name": <string>,
          "arguments": { ... },
          "results": { ... },
          "inference": <string>
      }
    ],
    "unresolved": ["..."],
    "usage": {
      "rounds": 3,
      "calls": 6
    }
  }
```

Version 1 shall emit exactly one compact JSON object followed by a newline on
standard output. No diagnostics or progress messages may be written there.
Diagnostics go to standard error and complete transcripts go only to the debug
file. Once argument parsing has started an invocation, runtime and model
failures shall also produce a valid result envelope. `--help` is the only mode
that writes non-JSON output to standard output.

Possible statuses are `answered`, `inconclusive`, `budget_exhausted`, `unstable`
and `error`. If budget is exhausted, the caller can increase the budget and try
again or try something else. The process shall exit successfully for `answered`,
`inconclusive`, `budget_exhausted`, and `unstable`. It shall exit nonzero only
for `error` or an invalid invocation; the controller shall use the JSON status
to distinguish normal investigation outcomes.

At the start of each `lava-agent` invocation, it shall require the replay service
to be paused and record its position. Immediately before returning its final
result, it shall check that the service is still paused at the same position. If
either check fails, it shall return `unstable` while preserving any evidence it
collected. It does not need to lock the service or poll repeatedly between these
two checks.

## Tools

`lava-agent` shall have its own compile-time allowlist of typed tools. It may
share lower-level implementations with `lava-tui`, but shall not reuse the TUI
tool registry or expose arbitrary `lava-cli` command strings. In particular,
the allowlist must not acquire replay-control tools merely because they are
available to the TUI.

Immutable facts, such as packets, objects, frames, and captured metadata, shall
come from the local trace file. Live facts, such as replay thread positions and
waits, deadlock diagnosis, and current memory state, shall come from the replay
service. Model-visible tool names shall use `trace_` and `replay_` prefixes so
that the provenance is apparent in evidence.

Service tools may call Vulkan and may use the service's exclusive-command mutex
as long as they do not progress any replay thread or change replay
configuration. Diagnostic results do not need to have been cached already.
The current `parameters THREAD` command shall not be exposed because selecting
a different thread can progress that thread; it can be reconsidered after that
behavior is fixed.

The initial replay-service tool allowlist is:

* `replay_get_status`
* `replay_list_threads`
* `replay_diagnose_deadlock`
* `replay_diagnose_device`
* `replay_get_memory`
* `replay_get_suballocator`
* `replay_get_object_state`
* `replay_get_instrumentation`
* `replay_get_as_build`

The initial local-trace tool allowlist is:

* `trace_get_metadata`
* `trace_list_threads`
* `trace_get_thread`
* `trace_get_frame`
* `trace_list_objects`
* `trace_get_object`
* `trace_get_packets`, for one packet or a bounded packet range
* `trace_find_calls`, for an exact command name with optional thread and packet
  range filters and a bounded number of results

The local tools shall not expose arbitrary files from the trace container.
Regex search is deferred.

Wall-clock duration and the size of the result returned to the cloud controller
are the user-facing budgets. The runtime shall also enforce generous internal
sanity limits on model rounds, total tool calls, individual and accumulated
tool-result sizes, packet ranges, and search hits. These internal limits protect
reliability and local-model context rather than optimize local-model token cost.
If the runtime cannot finish within its duration or internal budgets, it shall
return `budget_exhausted` with any useful partial evidence.

The default wall-clock limit is 300 seconds and the default structured-result
limit is 32768 bytes. The controller may override either limit.

The `lava-agent` runtime shall assign an immutable ID to each tool call and
record its validated arguments and actual result. The model may cite those IDs
and provide an inference, but it may not author the call or result records. The
runtime shall reject unknown evidence IDs and construct the final `evidence`
array from its own records. The model requests each next round of calls and does
the inference. The final `evidence` array shall contain only cited calls. When a
debug file is enabled, the runtime shall record the complete tool-call
transcript there, including exploratory calls that were not cited. The
`usage.calls` value still counts all calls.

# Cleanup

We probably should clean up and refector the TUI code a bit so that we can reuse
as much of it as possible.

# Future possibilities

* Have human readable markdown output as an option instead of JSON
  `-m/--markdown          Output in markdown format instead of JSON`
* Regex search capability for calls and logs
* Structured input as cmd line option
  `-i/--input FILE        Take prompt as structured JSON input instead`
* Allow not having a replay service running, with cmd line option
  `-l/--local-only        Do not connect to a replay service`
* A `go` mode (as opposed to `ask`) that uses a different system prompt and tool
  options to allow the agent to modify the running replay service, such as
  "go to the start of the first frame that is not a loading frame".
