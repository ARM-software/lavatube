---
name: validate-docs
description: Validate the lavatube documentation
metadata:
  short-description: Validate lavatube documentation
---

Review each of the following documents:

README.md - look for inconsistencies, maybe recommend things to put into separate documentation
SECURITY.md - look for inconsistencies
TODO.md - look for anything already implemented
AGENTS.md - look for anything wrong or that could be removed because it is already intuitive to an agent, we want this file short
doc/Goals.md - look for out of date info, check git log for last update and recommend redoing perf measurements if months old
doc/MemoryManagement.md - look for inconsistencies and out of date info
doc/Multithreading.md - look for inconsistencies and out of date info

For the goal metrics, the script `scripts/generate_metrics.sh` may be run to gather new metrics (ignore the performance metrics
for now).
