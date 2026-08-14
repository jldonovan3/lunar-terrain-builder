# Pre-Spec Design

## Purpose

Use this mode when the user wants to discuss or shape a development design before writing the spec. Inspect the repo, resolve only the choices that affect the result, recommend a right-sized plan, and ask for confirmation where it is actually needed.

Entering this mode does not by itself authorize spec edits or code edits. A recommendation is not a decision, and a question from the user is not an instruction to change files.

## Working Rules

1. Inspect applicable repo instructions, relevant entry points, existing facilities, callers, tests, public surfaces, and conventions before asking questions the repo can answer.
2. Use ordinary engineering judgment for private structure, naming, helper decomposition, routine library choices, and other decisions already settled by the repo or common practice.
3. Ask only when the answer changes behavior, architecture, data meaning, ownership or lifetime, public compatibility, acceptance, work order, operating requirements, or product policy.
4. When an important choice remains, recommend one option and explain its practical effect before asking the user to decide. Also recommend how deeply the plan should be split.
5. Use ideas from mature systems as evidence about tradeoffs. Adopt their structure or machinery only when it solves a current need and fits this repo.
6. Do not invent scale, concurrency, recovery, compatibility, extensibility, or operational requirements. A broad future need is context for avoiding an obvious dead end, not a current demand for a complete future architecture.
7. Make important agent-selected assumptions visible. Do not silently turn an inference into settled design.

## Method

### 1. Establish the whole

From the repo and the request, determine:

- the result the user wants and how it will be recognized;
- the existing facilities and real integration points;
- how one run moves from entry or input to result;
- important changes in data or state, identity, ownership, and lifetime;
- the code or component responsible for the operation;
- public behavior, failure rules, evidence-based compatibility obligations, and acceptance that must be stable.

Identify whether the task is new development, an incremental feature, a refactor, or a migration. Design new development from the target system and its real entry points. Before adding transition or compatibility work, identify the actual released surface, downstream user, persisted data, repo policy, or explicit commitment that requires it. Follow the compatibility guidance in `spec-architecture.md`.

Explain only the existing code and domain background needed to understand the design. Cite the rest.

### 2. Recommend the plan

Start from what must exist or be known before other work can be completed correctly. Group work that serves the same goal or responsibility. This normally produces the main order, but it need not make every neighboring task consume the previous task's output.

A clearly bounded task may sit beside the main order when its purpose, inputs, result, and checks are already known. If later integration or design must determine those details, keep only its goal in the future plan. Being related to the same feature does not by itself make a task ready.

Use the fewest levels that make the work understandable:

- A compact task can go directly to implementation steps.
- A larger task can use stages, and split the current stage further when useful.
- One or two middle levels are normally enough. Recommend more only when they clearly improve understanding, review, assignment, or resumption, and explain why.

Each current step must produce a result that can be checked without relying on unfinished future behavior. Do not plan future commits in detail before their design is current.

### 3. Resolve important choices

For each unsettled point:

1. Follow repo evidence when it answers the question.
2. Apply useful principles from established practice, adjusted to this repo and task.
3. Prefer the simplest local design that meets the current requirements.
4. If reasonable choices change the result or an important boundary, recommend one and explain the tradeoff.
5. Ask the user when product, domain, or personal policy is required.
6. Leave private implementation choices to the implementer.

A deferred important choice needs a safe current default and confirmation before the spec or code relies on it. If no safe current choice exists, the affected work is blocked.

### 4. Stop when the next step is clear

Stop expanding the design when the goal, whole flow, responsibilities, important contracts, current operating needs, acceptance, and work order are clear enough for the requested next step. Lower-level design that has not been reached is expected future work, not a defect to report.

## Report and Confirm

Use the reporting guidance in `spec-architecture.md`: establish the overall result and repo-based understanding, then show the breakdown, work order, and reasons. Apply the same reasoning inside each useful middle level.

For the whole task, also explain one run from entry or input to result, important contracts, acceptance, and verification. For a child level, state only its local goal and needed detail. Do not repeat the parent or create empty headings for a small task.

End with the important assumptions or defaults chosen by the agent, remaining questions, and the recommended planning depth. Ask for confirmation before authoring when the user requested discussion or assessment. A direct request to write or revise a spec authorizes that work after important choices are settled.

## Final Check

Before requesting confirmation, ask whether the repo was inspected, the whole precedes the parts, questions are genuinely important, the work order reflects real dependencies without forcing a perfect chain, future details remain future details, the proposed number of levels is justified, and every agent-made choice that could affect the result is visible.
