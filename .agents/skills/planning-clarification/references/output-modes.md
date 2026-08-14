# Output Modes

Match the output to the executor and the user's interest in implementation
detail.

## Human-facing brief

Prefer:

- Goal
- Scope
- Non-goals
- Constraints
- Unknowns or assumptions
- Recommended path
- Deliverables
- Completion criteria

Keep it easy to scan and decision-oriented.
When the user is mainly result-oriented, keep implementation detail secondary.

## Agent-facing brief

Prefer:

- Task goal
- Relevant context
- Explicit repository, path, or workspace context when it matters
- Scope and non-goals
- Behavior target, acceptance signal, execution path, and integration boundary
  when implementation order matters
- Constraints and operating rules
- Execution boundaries: allowed local actions and actions requiring explicit approval
- Expected output
- Completion criteria

Assume the receiving agent has no hidden context.
Do not assume the current working directory or session workspace is the later
execution directory unless that is explicit.

## Result-oriented execution brief

When the user mainly wants the final result, prefer:

- Goal
- Required inputs or local artifacts
- Explicit workspace or path scope when it matters
- Acceptance criteria
- Deliverables
- Stop-and-ask conditions
- Execution boundaries, especially limits on global installs, system changes,
  unrelated file access, or high-risk external actions

Keep implementation detail brief unless it is necessary to complete the task safely.

## Fresh-context prompt

When the user asks for a prompt for another agent:

- Make it self-contained and short.
- State the goal, current known state, requested work, deliverables, and stop
  conditions.
- Ask the receiving agent to inspect the current repo or supplied artifacts and
  confirm the approach before implementing when the task is repo-dependent.
- Use "current repo" or "current workspace" when the user will start the new
  session from the target project.
- Include an absolute path only when the user asks for it, when multiple repos
  are involved, or when the path is required to avoid ambiguity.
- Convert discussion corrections into positive settled constraints.
- Include only implementation details that are already settled requirements.
- Keep execution boundaries explicit when the later agent may act on local files,
  install tools, change environments, or take external actions.
- Preserve the receiving agent's independent confirmation step. Keep unverified
  design conclusions out of the prompt.

Keep these out of the prompt:

- rejected alternatives from the discussion unless they are active safety boundaries;
- correction-history clauses that only say a previous approach was wrong;
- fake third-party paths when the work will start from the target repo;
- long implementation recipes that should be rediscovered from local context.
