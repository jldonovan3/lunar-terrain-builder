# File Organization

## Purpose

Use the fewest files that keep the design easy to understand, execute, review, and resume. Planning levels do not automatically require separate files, and planned commits do not require separate briefs.

## Choose the Layout

Start with one spec:

```text
spec.md
```

It should contain the overall goal, how the system works for this change, the plan and its reasons, accepted contracts, current detail, and acceptance.

Add structure only for an observed need:

1. Add sections for stages when the task needs a middle planning level, keeping them in the same file while it remains clear.
2. Add a separate brief when the current implementation step must be assigned, approved, reviewed, or resumed independently.
3. Add a current pointer, progress record, or decision record only when a future executor could otherwise misunderstand the state.
4. Split a large stable spec by responsibility only when readers would otherwise have to load substantial unrelated detail.

Do not mirror the source tree, pre-create one file for every future stage, or add empty layers to match an example.

A larger task with genuine independent execution or resume needs might use:

```text
spec-root/
  overview.md
  plan.md
  briefs/
    current-step.md
  CURRENT.md
  progress.md
  decisions.md
```

Use only the files that solve a current problem and follow an existing repo convention when it already serves the purpose.

## Design, Briefs, and Current State

Stable design text states the current goal, whole-system behavior, shared contracts, responsibilities, plan, acceptance, and applicable execution boundaries. It describes the accepted design, not the discussion that produced it.

A brief narrows that design to one current step. It may restate the step's concrete obligation and point to the defining section, but it does not redefine shared rules.

When a durable pointer is necessary, keep it short:

```md
Current stage, if any:
Current step:
Current brief:
Blocked:
Verification:
Next action:
Return point after inserted work:
```

Omit fields and the file itself when the spec and repo already make the state obvious.

## Updating Files

Before editing, identify whether a file is active design, a current brief, a frozen fixture, or a resume record.

- When spec revision is in scope, correct wrong or unclear design text directly so a new reader sees the current design without reconstructing history.
- During implementation, do not change an accepted brief or frozen fixture unless the current task authorizes it.
- Record progress only for state a future executor needs.
- Record a decision only when an important accepted choice needs context beyond the corrected design text.
- Preserve unrelated user content and existing history.

When an accepted decision changes active requirements, update their defining section and affected briefs. Do not require readers to combine a stale spec with a decision log to discover what to build.

## Check

Before finalizing the layout, ask whether the goal and whole flow appear before local detail, every separate file has a practical reader or execution need, a fresh executor can find the current and next work without chat history, every shared rule has one defining place, and records make current state easier rather than harder to find.
