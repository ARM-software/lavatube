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

# Security

The agent is sandboxed and only allowed a very limited set of tool calls. It
has strict limits on how many rounds, tool calls, wall clock time, output
bytes and tokens it is allowed to consume.

The replayer service it calls is also sandboxed.

This leaves the model service endpoint as the only component that the model
interfaces with that is not secured from our side.

# Tests

Questions to pose the agent for testing its efficacy can be found in
[this doc](Agent.QA.md).
