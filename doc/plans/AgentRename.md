# lava-agent rename

The word agent has become synomymous with things like openclaw
and other agentic AIs with very wide permissions and massive
risks that management tends to get anxious about. It would be
good to distinguish what lava-agent does from these things.

At the same time, I don't want to hide the fact that it is an
AI tool and does consume tokens. Also its primary use is likely
that it is called from another AI harness using a more expensive
model which can fire off a number of lava-agent queries in the
background. In that sense it is a 'tool', but that word is
already very overloaded. So there does not seem to be any existing
terminology that covers such type of tools, from what I can tell.

Best new word seems to be 'reasoning probe', which means I should
rename it to 'lava-probe'. The rename only needs to be skin deep.

In short, it is a sandboxed reasoning probe - a model-driven
component that a parent AI controller dispatches to perform
token-consuming, multi-step trace introspection without granting
host permissions and spending less tokens than a parent AI would
for the same task.
