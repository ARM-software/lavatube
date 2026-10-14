---
name: validate-docs
description: Audit lavatube documentation for stale claims, inconsistencies, and unnecessary agent instructions.
---

# Validate documentation

Respect the requested scope: a focused document review needs only relevant files; a repository-wide audit covers the
following documents. Review without compiling or running tests; edit only when requested.

| Document                                           | What to verify                                                                                       |
| -------------------------------------------------- | ---------------------------------------------------------------------------------------------------- |
| `README.md`, `SECURITY.md`                         | Claims, commands, and boundaries against current code; identify detail better placed in focused docs |
| `TODO.md`                                          | Items already implemented or no longer applicable                                                    |
| `AGENTS.md`                                        | Incorrect or redundant instructions; retain project-specific invariants and useful entry points      |
| `doc/Goals.md`                                     | Whether metrics still describe current code and conditions; use git history to establish their age   |
| `doc/MemoryManagement.md`, `doc/Multithreading.md` | Design descriptions against current implementation                                                   |

Report discrepancies with document and implementation locations. For old metrics, identify what would need remeasurement
rather than treating age alone as proof they are wrong. `scripts/generate_metrics.sh` includes performance runs and
capture side effects; do not run it during a documentation review. Use it only when refreshing metrics is in scope.
