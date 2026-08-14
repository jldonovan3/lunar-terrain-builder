# Progress and Decisions

## Purpose

Keep records only when a future session or executor needs them to continue correctly. Progress states where the work is. Decisions preserve an important accepted choice when the corrected spec alone would not retain enough context. Neither file is a version history, conversation log, review log, or implementation diary.

## Decide Whether to Record Anything

Use this order:

1. If design text is wrong or unclear and revision is authorized, correct its defining section directly.
2. If the current work, blocker, verification state, next action, or return point changed in a way a future executor must know, update progress.
3. If an accepted choice affects future work and its reason or scope would be lost from the corrected spec, record a decision as well.
4. Otherwise record nothing.

Do not preserve private names, helper structure, local cleanup, wording polish, review dialogue, rejected minor alternatives, temporary investigation, compatibility trivia, or code-level nits.

## Current State and Progress

When the work spans sessions and the state is not obvious, keep one easy-to-find pointer in `CURRENT.md`, `progress.md`, or the repo equivalent:

```md
Current stage, if any:
Current step:
Current brief:
Blocked:
Verification:
Next action:
Return point after inserted work:
```

Omit the pointer for compact work when the spec and repo already make the next action clear.

Record only changes that affect continuation, such as a step becoming current or complete, a milestone enabling later work, a blocker appearing or clearing, verification changing what can proceed, or inserted work changing the next action. State the result, its effect on continuation, and the next action in a few lines. IDs, dates, and fixed fields are optional unless the repo requires them. Do not write one entry per commit.

Preserve unrelated existing entries. If the repo treats a record as append-only, add a short superseding entry rather than rewriting history.

## Decisions

Record a decision when an accepted choice changes future implementation and direct correction of the spec would not preserve context needed to apply it. Examples include behavior, public interfaces, shared assumptions, data meaning, responsibility or ownership, input guarantees, failure, fixture interpretation, acceptance, work order, edit boundaries, or the intended operating range.

State the choice and only the reason, scope, and follow-up needed to use it. Include a stable ID only when other records must refer to it. Write the decision before implementation relies on an otherwise unspecified important choice. If no safe choice exists, block the affected work instead of guessing.

When a decision changes active requirements, also update their defining section and affected briefs. A future executor should not have to merge a log with stale design text.

## Frozen Fixture Conflicts

A frozen fixture remains unchanged during implementation. If it conflicts with accepted requirements:

- continue without the affected case only when the remaining evidence is still valid and an accepted decision permits it;
- block when the contradiction affects the only reliable acceptance evidence;
- record the accepted interpretation and follow-up, then change the requirement or fixture only through an authorized revision.

Do not silently code to the contradiction or rewrite the fixture as ordinary implementation work.

## Check

Before adding a record, ask whether it changes what a fresh executor should do, whether the current design is already clear in its defining section, whether current work and next action can be found without chat history, and whether the entry preserves an important decision rather than the process used to reach it.
