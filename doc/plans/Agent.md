# lava-agent

New agentic experiment based on experiences from `lava-cli` and `lava-tui`.

Where `lava-cli` became a useful but primitive diagnostic tool, `lava-tui`
became unused because it was not as powerful as existing agent harnesses.
The useful parts of `lava-tui` - its close tool integration, prompt control
and local model support - can perhaps be utilized better in a different
type of tool.

`lava-agent` is meant as a subagent that operates with a limited number of
steps on a single query and reports back its findings in a structured format.

It requires:
* Local access to the trace file
* Access to a running `lava-replay` service instance that it can query.
* Access to an AI model supporting an OpenAI compatible interface (like `lava-tui`)
* A fixed system prompt that tells it what it can and must (not) do
* A specific user prompt that tells it what to investigate
* Structured JSON output
* Being able to check with `lava-cli info` that our local trace file is the
  same as the running replay service.

It may have:
* Ability to look into a trace file similarly to `lava-print` (TBD, see
  `PrintPerformance.md` for ideas)
* Access to system logs

It must not:
* Modify a running `lava-replay` service instance. The list of tool commands
  must not include options to do so.
* Require any state or history. Any context it needs should be provided by
  the user or calling model.
* Run concurrently with itself, `lava-tui` or `lava-cli` sessions targeted
  at the same replay service, as it has no support for concurrent users.

We do not want:
- Persistent history
- Replay control
- Arbitrary commands
- Trace modification
- Human chat UI

Future possibilities:
* Be able to run on Android alongside a replay there. In this case it would
  be started and instructed over adb.
* Have human readable markdown output as an option instead of JSON as cmd line option
  `-m/--markdown          Output in markdown format instead of JSON`
* Regex search capability for calls and logs
* Ability to query the local system configuration (similarly to `vulkaninfo`)
  through `lava-cli info device` (to be sure to get the right GPU)
  and `lava-cli info system` (for host info)
* Ways to limit model budget more carefully
* Structured input as cmd line option
  `-i/--input FILE        Take prompt as structured JSON input instead`
* Allow not having a replay service running, with cmd line option
  `-l/--local-only        Do not connect to a replay service`

## Interface

The simplest request would be something like:

`lava-agent example.api "Is thread 3 blocked on work from another replay thread?"`

This would connect to a replay service on localhost and load `example.api`
(as needed) to answer the posed question.

The prompt implies that there is a running replay service that we can look at,
and we should check the trace status then investigate the trace file to find if
the suggested state is plausible. Then report whatever evidence turned up.

There could be the following options:

```
lava-agent 0.0.2-debug command line options
lava-agent [options] <trace file> <prompt>
-h/--help              This help
-v/--verbose           Verbose output (if debug file enabled)
-d/--debug level       Set debug level [0,1,2,3] (if debug file enabled)
-df/--debugfile FILE   Output debug output to the given file
-H/--host              Hostname to connect to (default is localhost)
-p/--port              Port number to connect to (default 11901)
-r/--max-rounds        Maximum number of tool call rounds (default 8)
```

It should return structured evidence along the lines of:

```json
  {
    "status": "answered",
    "question": "...",
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
    "target": {
      "trace": <trace filename>,
      "service": "localhost:11901"
    },
    "usage": {
      "model_rounds": 3,
      "tool_calls": 6
    }
  }
```

Possible statuses are `answered`, `inconclusive`, `budget_exhausted`, `unstable`
and `error`. If budget is exhausted, the caller can increase the budget and try
again or try something else. It shall return `unstable` if it detects another
process likely modifying a live replay service (or it is not in a stable paused
state).

`lava-agent` shall record the tool calls and results, while the model does the
inference and request the next round of calls.
