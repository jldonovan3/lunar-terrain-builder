---
name: spec-driven-dev
description: >
  Discuss, write, refine, review, and implement repo-aware development specs.
  Use after a rough requirement or plan exists to settle important design
  choices, produce a concise execution-ready spec, or implement a supplied
  spec in gated copilot or explicitly authorized autonomous mode.
---

# Spec-Driven Development

## Purpose

A development spec tells an implementer what to build, how it fits the repo, what must remain true, how the work should proceed, and how completion will be checked. It is not a tutorial, code tour, architecture essay, or duplicate of existing documentation. Cite existing sources and include only what this change needs.

Carry work from design discussion through a concise spec to repo-native implementation and verification. Enter at the point supported by the user's request and current files; a task does not need every mode.

## Core Method

Start with the whole. Inspect the repo, state the actual goal, and explain how one run moves from entry or input to its result. Include only the data, state, ownership, lifetime, and runtime organization needed to understand this change.

Then plan the work. Most work should follow what must exist or be known first. Group work that serves the same goal or responsibility, but do not pretend every task must consume the previous task's output. A task may sit beside the main sequence when its purpose, boundary, and result are already clear. If later work must decide those details, leave the task at outline level.

Use the fewest planning levels that keep the work clear. A small task can go directly to implementation steps. Larger work usually needs no more than one or two middle levels; use more only with a clear reason. The agent recommends the split and its reasons for confirmation. When reporting the architecture or explaining a supplied spec, start with the overall result and repo-based understanding, then show the breakdown, work order, and reasons. Use the same logic inside a meaningful middle level without forcing headings on a simple task.

Work at the level the user requested. During architecture discussion, lower-level details that have not been reached are expected later work, not defects. Discussion, a recommendation, or a question does not authorize writing a spec or editing code.

Review or revise the current document against the confirmed goal, repo evidence, whole document, and current design level. Report a finding or revise only when the text, read in that context, shows a concrete mismatch or permits materially different delivered behavior, architecture, work order, verification, scope, acceptance, or safe continuation. A review may correctly find no substantive issue.

## Modes

Choose the mode or confirmed sequence of modes from the request and current files:

1. **Pre-Spec Design.** Inspect the repo, settle important choices, recommend the plan, and present it for confirmation. A request for discussion stays here; an explicit request to design and write may continue after important choices are settled.
2. **Spec Authoring.** Write, revise, organize, or review the smallest repo-aware spec that can guide correct work.
3. **Implementation.** Implement a supplied or identifiable spec for the current repo. Use gated copilot by default and autonomous execution only after explicit authorization.

A compact request with a settled goal, boundary, and acceptance may proceed without creating a separate spec file.

If `planning-clarification` is available and the request is too vague for spec-focused design, it may be used first. Otherwise clarify only the missing goal, scope, or deliverable.

## Hard Rules

These rules always apply. Read the referenced procedure before doing that work; use only the sections relevant to the task.

**Establish the whole.**

1. **Read the repo before designing.** Inspect applicable repo instructions, relevant entry points, existing facilities, code, tests, public surfaces, and conventions before settling repo-specific design. If inspection is unavailable, say which conclusions are provisional. Repeat this check before implementation. (See `references/spec-architecture.md`.)
2. **Explain the whole before the parts.** Put the target and the path from entry or input to result before component detail. Include important branches and changes in data, state, identity, ownership, or lifetime. Explain only the part of the existing system needed for this change. (See `references/spec-architecture.md`.)
3. **Plan from real dependencies, with judgment.** Give every spec a recommended order based mainly on what must exist or be known first. Group related work, allow clearly defined tasks that do not depend on adjacent work, and leave details that depend on later design unsettled. Use only the planning levels the task needs. (See `references/spec-architecture.md` and `references/execution-briefs.md`.)
4. **Confirm important choices before writing or editing.** Resolve ordinary choices from repo evidence. Ask only when the answer changes behavior, architecture, data meaning, lifecycle, public compatibility, acceptance, planning, or policy. Show important defaults chosen by the agent. (See `references/pre-spec-design.md` and `references/implementation.md`.)

**Write the smallest complete design.**

5. **Design the requested system at the current depth.** For new development, design the target system rather than an imagined migration. Base compatibility on real users, released surfaces, persisted data, or explicit commitments rather than the mere existence of code. Borrow ideas from mature systems only when they solve a current need. A broad future scale estimate calls for sensible choices, not a large-scale architecture; specific optimization normally becomes separate work based on measurements. (See `references/spec-architecture.md`.)
6. **Fix the contract; leave code choices open.** State behavior, acceptance, public interfaces, what components may assume about one another, ownership, lifecycle, conditions that must remain true across components, and required development dependencies when different readings would change the result. Leave private helpers, local control flow, internal data structures, and ordinary repo-native choices to implementation. (See `references/writing-principles.md`.)
7. **Define each common rule once.** Give every shared rule or concept one defining place. Other sections point to it. A self-contained implementation brief may restate what its current step must do, but it must not redefine the shared rule. (See `references/writing-principles.md`.)
8. **Put task content and decision order first.** Arrange the spec so the accepted goal, whole-system view, parts, order and reasons, contracts, current detail, and acceptance follow naturally as neutral, task-specific content. Follow this skill, applicable working-context instructions and constraints, and repo rules throughout the work; include only their task-specific consequences in the spec. Use plain domain language and only formatting, terms, and rationale that preserve a real decision. (See `references/writing-principles.md`.)

**Preserve the whole during execution.**

9. **Execute only honest, approved scopes.** A stage or implementation step claims only behavior it can complete and verify now. Gated execution waits for approval of the current work. Autonomous execution requires explicit authorization and one confirmed outline. Corrections preserve the overall goal and next planned work. (See `references/implementation.md`.)
10. **Revise the whole design directly and keep records sparse.** When spec revision is authorized, keep the spec as one current design by integrating each correction into its defining section and deleting or coherently rewriting superseded text. Read the whole document before and after revising, update sections affected by the corrected definition, and inspect the diff to confirm top-down coherence, preserved decisions, and unchanged meaning outside the intended scope. Record progress only when a future session needs changed continuation state, and record a decision only when an important accepted choice is not already clear in the corrected spec. (See `references/progress-and-decisions.md`.)

## Reference Routing

- `references/pre-spec-design.md`: repo-based design discussion, planning recommendation, confirmation, and summary.
- `references/spec-architecture.md`: whole-system explanation, work planning, new-development posture, and design depth.
- `references/writing-principles.md`: concise requirements, contract detail, code freedom, and acceptance.
- `references/file-organization.md`: the smallest useful file layout and resume state.
- `references/implementation.md`: current scope, gated or autonomous execution, corrections, commits, and completion.
- `references/execution-briefs.md`: a standalone brief for the current implementation step when one is needed.
- `references/progress-and-decisions.md`: sparse records for resuming work.
- `references/self-check.md`: risk-based review before finalizing or revising a spec.

## Self-Check Output

For a Chinese self-check-only request with no requested format, output exactly `未发现实质问题` when no substantive issue is found.
