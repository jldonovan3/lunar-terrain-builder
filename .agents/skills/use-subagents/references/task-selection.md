# Task Selection

Use subagents selectively. The default remains a direct main-agent pass.

## Good Fits

- Independent review of a non-trivial artifact
- Design or algorithm critique where alternative interpretations matter
- Competing debugging hypotheses
- Targeted verification that benefits from a fresh context
- Research analysis with separable evidence gathering
- Bounded repository inspection that supports a larger task
- An implementation slice with settled behavior and independent acceptance

## Weak Fits

- Small local edits with obvious acceptance
- Rote commands or file manipulation
- Strongly sequential work
- Tasks blocked on missing requirements or repository context
- Helper-level implementation without independent behavior or verification
- Work where another context is unlikely to add evidence or coverage

## Pattern Choice

- **No subagent:** routine, tightly coupled, or low-risk work.
- **One subagent:** one independent review, bounded analysis, or separable
  support task.
- **Two independent subagents:** high-value review or ambiguous reasoning where
  comparison is worth the coordination cost.

More reviewers do not create independence if they receive the same anchored
prompt or one another's conclusions.

## Fresh Context

Prefer a fresh context for independent review, design critique, or contradiction
checking. Reuse context only when continuity is more valuable than independence
and the accumulated conclusions will not bias the task.
