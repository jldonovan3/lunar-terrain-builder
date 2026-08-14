---
name: planning-clarification
description: >
  Clarify vague or partially specified requests into concise, right-sized
  execution briefs or fresh-start prompts. Use for discussion-level planning,
  scoped questions, source-backed uncertainty handling, and neutral prompts for
  a new session. Do not use for persistent on-disk workpacks.
---

# Planning Clarification

## Purpose

Turn an imprecise request into a plan that is clear enough to execute and no
broader than necessary. Separate confirmed facts, working assumptions, decision
surfaces, and material unknowns before asking questions or drafting a brief.

This skill is discussion-only. Do not choose or enforce an on-disk doc format
here. If the user explicitly asks for a durable on-disk handoff workpack, switch
to the `plan-progress-tracker` skill.

Apply it to concrete, outcome-oriented tasks: engineering work, lightweight
automation, local artifact handling, structured organization, analysis, and
other tasks an agent may later execute end to end.

## Priority Model

Resolve conflicts in this order:

1. User-stated constraints, safety requirements, repo/project instructions, and
   supplied artifacts.
2. Core invariants: right-sized scope, explicit uncertainty, non-speculative
   requirements, and useful execution boundaries.
3. Task-shape guidance from the routed references.
4. Optional polish, formatting preferences, and follow-up ideas.

Do not upgrade a small or local task into architecture work, process design, or
defensive future-proofing unless the user explicitly wants that.

## Core Invariants

1. **Smallest sufficient planning surface.** Produce only the structure needed
   to let execution start correctly.
2. **Confirmed facts stay separate from assumptions.** State what the user
   actually said, what is inferred, and what remains unknown.
3. **Questions must change execution.** Ask only questions whose answers affect
   scope, deliverables, execution boundaries, output shape, or acceptance.
4. **Inspection is demand-driven.** Inspect local context only when relevant
   artifacts are available and the inspection can remove avoidable questions or
   materially sharpen the brief.
5. **Research only when it matters.** Browse or cite external facts when the
   plan depends on current tool behavior, standards, APIs, regulations, product
   capabilities, version-sensitive guidance, or precise source-backed claims.
6. **No speculative requirements.** Do not fill gaps with invented architecture,
   risk posture, maintenance assumptions, or implementation details.
7. **Fresh-context prompts preserve independence.** Include settled constraints,
   but ask the receiving agent to inspect and confirm repo-dependent context
   instead of inheriting unverified conclusions.
8. **Planning stops when the practical path is clear.** Do not keep questioning
   once remaining uncertainty no longer changes the next executable step.
9. **Implementation plans need a spine.** For feature development or refactor
   planning, name the behavior target, acceptance signal, owning execution
   path, and integration boundary before decomposing implementation tasks. If
   one is unknown, make locating or deciding it the next planning step.

## Structured Clarification Loop

Use this loop for vague, broad, partially specified, or handoff-oriented
requests. For clear requests, run it implicitly and emit only the needed answer
or brief.

1. **Classify the request shape.** Identify whether the task is implementation,
   automation, local artifact handling, structured organization, analysis,
   source-backed planning, or fresh-context prompting.
2. **Name the current understanding.** Extract the apparent goal, target
   artifacts or workspace, expected outcome, likely executor, and output shape.
3. **Separate facts, assumptions, and unknowns.** Do not present inferred
   requirements as user-confirmed facts.
4. **Identify decision surfaces.** Decide which choices still matter: scope,
   deliverable, non-goals, execution environment, ownership, acceptance,
   research basis, or level of process detail.
5. **Choose the evidence path.** Inspect local context when it is relevant and
   available; research external facts when they affect the plan; otherwise
   continue with explicit assumptions.
6. **Filter unknowns.** Keep only unknowns that change execution. Ask one to
   three high-leverage questions when a critical gap remains.
7. **Right-size the plan.** Prefer concrete scope, deliverables, acceptance,
   stop conditions, and operating boundaries over elaborate process.
8. **Produce the brief or prompt.** Shape it for the executor: human, current
   agent, or fresh agent context.
9. **Self-check the result.** Treat plans as incomplete if they invent
   requirements, overdesign, hide assumptions, ask low-value questions, or
   anchor a future agent on unverified implementation detail.

## Decision Gates

- **Local context gate:** Inspect files, repos, manifests, notes, or supplied
  artifacts only when the request makes them relevant. Do not treat the current
  working directory as authoritative by default. Inspect incrementally instead
  of recursively reading a whole repository.
- **Question gate:** Ask only the smallest set of questions that changes the
  plan. Prefer boundary-setting questions about deliverable, scope, non-goals,
  execution owner, environment, constraints, or acceptance.
- **Research gate:** Use authoritative sources when external facts affect the
  plan and the user has not forbidden browsing. If browsing is forbidden, plan
  inside that constraint and mark affected facts as unverified.
- **Uncertainty gate:** If a critical gap cannot be resolved by local inspection
  or research, ask the user directly. If the gap does not block execution,
  proceed with a provisional brief and label the assumption.
- **Output-shape gate:** Ask whether the brief is for a human or another agent
  only when that is not already clear and it materially changes the output.
- **Fresh-context gate:** Include a concrete path only when the user asks for
  it, multiple repos are involved, or the path is required to avoid ambiguity.
  Use "current repo" or "current workspace" when the later session will
  start from the target project.
- **Implementation planning gate:** For development or refactor work, do not
  produce an implementation task list until the behavior target, acceptance
  signal, owning execution path, and integration boundary are named. If they are
  unknown, inspect context, assign evidence-gathering steps, or ask targeted
  questions before decomposing implementation work.

## Reference Routing

Load only the files needed for the current clarification problem:

- `references/loop-pattern.md`: use when deciding whether to inspect, ask,
  research, proceed provisionally, or stop planning.
- `references/questioning-rules.md`: use when selecting high-leverage questions
  or deciding whether process detail vs result orientation matters.
- `references/research-and-uncertainty.md`: use when external evidence,
  source confidence, or unresolved uncertainty affects the plan.
- `references/right-sizing.md`: use when the draft brief risks overdesign,
  overdefense, or speculative future-proofing.
- `references/output-modes.md`: use when shaping human-facing briefs,
  agent-facing briefs, result-oriented execution briefs, or fresh-context prompts.

## Output Contract

When clarification remains, start with a concise current-understanding summary,
then ask only questions that materially change execution.

When the plan is clear enough, produce a provisional or final brief using the
needed subset of:

- goal;
- confirmed facts;
- working assumptions or unresolved unknowns;
- scope and non-goals;
- required inputs, workspace, or local artifacts;
- execution boundaries;
- deliverables;
- acceptance or completion criteria;
- stop-and-ask conditions.

For agent-facing execution briefs, state the default execution boundary: work
within the current workspace or supplied local artifacts; do not install global
tools, modify the system environment, touch unrelated locations, or take
high-risk external actions without explicit approval.

For fresh-start prompts, keep the prompt neutral and short enough for the
receiving agent to perform its own local confirmation pass. Collapse discussion
corrections into positive settled constraints. Include implementation detail
only when it is already a settled requirement.

## Self-check

Before finalizing, verify:

- confirmed facts, assumptions, and unknowns are not mixed;
- every question asked would change scope, deliverable, boundary, output shape,
  or acceptance;
- local inspection and external research were used only when relevant;
- the brief is no broader than the user's request requires;
- unresolved uncertainty is explicit rather than hidden;
- development or refactor work items either locate missing repo evidence or tie
  to a behavior target, acceptance signal, execution path, or integration
  boundary;
- fresh-context prompts do not preserve unnecessary transcript history,
  rejected alternatives, fake paths, or unverified implementation recipes;
- the task has not drifted into persistent workpack creation, framework
  internals, domain science, or detailed implementation when another skill or
  direct execution would be more appropriate.
