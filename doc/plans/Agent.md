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

It requires local access to the trace file and to an already-running replay
service. The caller owns the service lifecycle and replay position.

It will give you a atructured JSON output as a result.

It does not have a persistent history and is unable to run arbitrary commands.

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
* Access local models through LiteRT-LM, this may be needed on Android. It can
  offer up an OpenAI compatible endpoint, but unsure if this is the best way.

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
