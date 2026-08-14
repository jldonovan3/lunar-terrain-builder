# Spec Implementation

## Purpose

Use this reference when the user asks to implement, start, continue, or complete a supplied or identifiable spec for the current repo. The spec may be in the repo or supplied in the conversation; a compact request may itself provide enough design when its goal, boundary, and acceptance are settled. Do not create a spec file solely to enter this mode. Understand the whole design, then execute only the current authorized work.

## Before Editing

Read applicable repo instructions, the main spec, the current brief or status when present, and relevant code and tests. Confirm that the spec still matches the repo.

Separate what you read into three groups:

- **Requirements:** behavior, acceptance, public interfaces, assumptions shared between parts, data meaning, ownership and lifetime, failure rules, input guarantees, operating limits, and conditions that must remain true across parts or steps.
- **Repo choices:** private entities, helpers, files, control flow, naming, comments, routine library use, and ordinary test structure.
- **Explanation:** rationale, diagrams, examples, planning labels, and illustrative shapes.

Implement the requirements. Choose private code from the repo and the simplest design that fits. Use explanation to understand the design, not as mandatory production names or a code skeleton.

Reinspect the actual entry points, existing facilities, callers, tests, public surfaces, and conventions before settling implementation. A naming or private-structure difference is not a spec conflict. Stop when repo evidence contradicts any accepted requirement or contract, or when proceeding would require an important change to approved scope or product policy, and report the exact consequence.

## Choose the Current Work

Use the shallowest plan that fits:

1. Follow an explicit current brief or implementation step.
2. For a compact approved task, execute its direct steps in the order required by the repo and the work.
3. For a larger task, choose a ready step in the current stage. Most work follows what must exist or be known first; a separately bounded step may proceed when the accepted plan or current user focus gives it priority.
4. If no current pointer exists, use the spec, repo, and verification state to find unfinished work whose result can be completed and checked now.
5. If different scopes with important consequences remain plausible, ask one focused question before editing.

Do not inventory unresolved details from future stages. They are not current defects. Do not start a related task merely because it shares a stage; its boundary and result must already be knowable without guessing later design.

## Summarize the Design When Asked

When the user asks for the architecture, main plan, or your understanding of a supplied spec, follow the reporting guidance in `spec-architecture.md`: establish the overall result and repo-based understanding, then show the breakdown, work order, and reasons. Apply the same reasoning inside each useful middle level.

At the whole-task level, include how one run moves from entry or input to result, important contracts, acceptance, and verification. At a child level, state only its local goal and needed detail. Keep this an execution summary, not a retelling of the spec.

## Gated Execution

Use gated execution unless the user explicitly authorizes autonomous execution. Discussion, a recommendation, approval of an architecture, or a stage outline does not authorize unspecified code changes.

The normal approval unit is one reviewable implementation step. An explicit request to implement a supplied spec authorizes entering this workflow and starting its first concrete step after the pre-edit summary; it does not authorize later steps or autonomous continuation. A compact task, bounded sequence, or whole stage may instead be the current unit when the user's request names it and its concrete work and review boundary are clear.

Before editing, summarize:

- the current goal and its place in the plan;
- accepted behavior and relevant repo evidence;
- expected code, test, and necessary collateral edits;
- acceptance and focused verification;
- frozen or approval-sensitive boundaries;
- intended local commit behavior;
- important risks or conflicts.

If this concrete scope is not already approved, wait. Then implement only that scope, run focused verification, compare the result with the spec and repo, and report changes and remaining risks.

After the user accepts the result, update design text only when revision was included in the approved scope, and update records only where needed. Follow the commit behavior established by the user's request, repo instructions, or the approved execution plan; when a local commit is called for, keep it focused and do not stage unrelated changes. Identify the next step and wait for its approval.

## Autonomous Execution

Use autonomous execution only after explicit authorization. Confirm one overall outline containing the goal, current repo state, grouped work, real dependencies, any separate ready work, recommended order, fixed boundaries, acceptance, verification, commit behavior, and conditions that will stop execution. If the authorization already covers that exact outline, proceed; otherwise present it and wait.

For each reviewable step:

1. Re-read its requirements and relevant repo context.
2. Implement the smallest complete result that advances the confirmed goal.
3. Run focused verification and check behavior, scope, repo fit, and unnecessary complexity.
4. Correct issues that stay inside the confirmed design.
5. Update only records needed to resume accurately.
6. Create a focused local commit without staging unrelated changes when the confirmed outline calls for one.
7. Continue to the next confirmed step.

Stop when necessary work leaves the confirmed scope, changes an accepted contract or acceptance, lacks a safe product or domain answer, conflicts with user changes, or makes the confirmed design unworkable. Do not stop for ordinary private implementation choices or repo-consistent corrections.

Autonomous authorization does not include pushing, opening a pull request, releasing, deploying, publishing, or other external effects unless separately authorized.

## Corrections and Completion

Correct implementation, private naming, tests, comments, or local complexity directly when accepted behavior is unchanged. Confirm changes to behavior, public surfaces, data meaning, ownership, input guarantees, failure, acceptance, work relationships, or operating requirements before applying them to the main spec and affected future work.

When the accepted design includes temporary compatibility, preserve its stated migration and removal condition. Do not let temporary coexistence become an unintended permanent second implementation.

Do not turn each review comment into a permanent rule or new stage. For inserted work, preserve the next planned action and record a return point only when a future session needs it.

The current scope is complete when its promised behavior exists, focused checks pass or limitations are reported accurately, no important spec conflict remains, review and commit expectations are satisfied, and the next action or final result is clear.
