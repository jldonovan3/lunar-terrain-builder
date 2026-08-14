---
name: plan-progress-tracker
description: >
  Create and maintain a minimal repo-local plan and progress workpack for
  development that must continue across sessions or agents. Use when durable
  handoff state, a shared task inventory, or material decisions must persist on
  disk. Do not use for discussion-only planning, a standalone spec, or routine
  work whose current state is already obvious from the repo.
---

# Plan Progress Tracker

## Purpose

Maintain the smallest durable documentation set that lets later work resume
without reconstructing chat history. Preserve current direction, actionable
state, and material decisions; do not turn the workpack into a second repository
of general instructions, implementation notes, or activity history.

For requirement clarification, use `planning-clarification`. For design,
authoring, or implementation of a development spec, use `spec-driven-dev`.
This skill tracks durable work state; it does not define production structure or
replace repo instructions.

## Hard Rules

1. **Use a workpack only for durable handoff.** Create or expand one only when
   work will be resumed across sessions or agents and the repo or existing spec
   does not already make the current state sufficiently clear.
2. **Persist only future-useful information.** Record information needed to
   resume work, choose the next action, verify completion, or preserve a
   material decision. Omit conversation history, review sequences, routine
   activity, transient investigation, superseded local details, and information
   recoverable cheaply from authoritative repo sources.
3. **Content determines structure.** Use the fewest files and sections that keep
   the current work clear and maintainable. Templates are optional section
   menus, not completeness requirements. Do not create empty, placeholder, or
   parallel documents merely to satisfy a layout.
4. **One fact has one authoritative owner.** Link instead of duplicating goals,
   requirements, task definitions, state, or decisions. Existing repo docs and
   specs remain authoritative for their own content.
5. **Stable text states the current effective result.** Correct canonical
   content directly when history is unnecessary. Preserve decision history only
   when a changed choice still affects future judgment, auditability, or an
   unresolved dependency.
6. **Status is not a diary.** Keep only the current position, blockers, next
   action, and necessary links. Do not retain completed micro-steps, review
   chatter, temporary notes, or narrative progress.
7. **Document boundaries are not production boundaries.** A topic or
   responsibility described separately does not imply a production module,
   type, interface, or file. Create a separate topic document only when it has
   substantial independently maintained content or an independent handoff need.
8. **Keep the workpack portable.** Prefer repo-relative paths and authoritative
   links. Persist machine-specific absolute paths only when the path itself is
   an external requirement.
9. **Stay within documentation scope.** Edit implementation only when the user
   separately requests it.

## Modes

Classify the requested operation before editing:

- **Initialize:** create a workpack in an explicit non-existent or empty target.
- **Update:** change an identified existing workpack while preserving unrelated
  content and its established layout.
- **Reconcile:** repair contradictory current state, task ownership, links, or
  canonical content across the workpack.
- **Handoff:** reduce current state to what a fresh agent needs to resume.

If the target or scope is materially unclear, ask only the question needed to
determine whether, where, or how to write the workpack.

## Select the Minimal Document Set

Use repo conventions when they already provide an adequate shape. Otherwise
select only the needed roles:

- **Entry:** where to start and which documents are authoritative. Omit when one
  file is self-explanatory.
- **Overview:** durable goal, scope, important constraints, acceptance, and the
  high-level execution path. Omit when an existing spec owns this information.
- **Plan:** the shared inventory of remaining work. Use when several durable
  tasks or milestones must be coordinated.
- **Status:** current task or milestone, blockers, next action, and essential
  links. Use when this state is not already obvious from the plan or spec.
- **Decisions:** material choices that are not fully represented by corrected
  canonical text and still affect later judgment.
- **Topic documents:** substantial independently maintained designs or areas.
  Do not create them for every logical responsibility or task.

A single `PLAN.md` or `STATUS.md` is valid when it is sufficient. A larger
workpack is valid only when each file has distinct durable ownership.

## Workflow

1. Read applicable repo instructions, the target workpack, linked specs, and
   the relevant repository state.
2. Identify the durable information future work actually needs.
3. Select or preserve the smallest fitting document structure.
4. Update canonical information before dependent status or links.
5. Keep one task inventory when a plan exists; status may reference tasks but
   must not create a parallel list.
6. Record a decision only when it remains necessary after canonical text is
   corrected.
7. Remove or collapse stale transient content when the requested update
   authorizes maintaining that file.
8. Check links, current state, blockers, next action, and authoritative
   ownership before reporting the handoff.

## Reference Routing

- `references/workpack-layout.md`: use to select document roles, determine
  ownership, and reconcile an existing layout.
- `references/templates.md`: use as optional compact shapes when creating or
  repairing a document. Include only sections justified by current content.

## Self-check

Before finalizing, verify:

- the task genuinely needs durable on-disk handoff;
- every file and section has distinct future value;
- current state and next action are clear without activity history;
- requirements, tasks, status, and decisions are not duplicated;
- corrected canonical text stands on its own where history is unnecessary;
- topic documents reflect documentation needs rather than assumed production
  structure;
- paths remain portable unless an absolute path is required;
- implementation and unrelated repo content were not changed.
