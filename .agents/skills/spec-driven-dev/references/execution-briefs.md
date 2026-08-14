# Execution Briefs

## Purpose

Create a separate execution brief only when the current implementation step must be assigned, approved, reviewed, or resumed independently. A brief turns the accepted spec into a compact current assignment; it does not redesign the system, teach the repo, or prescribe private code structure.

Keep the brief in the main spec when a separate file adds no practical value.

## Place the Step in the Plan

Before detailing the step, state:

- its goal and the larger goal it serves;
- what must already exist or be known;
- why this is useful to do now;
- the complete result it will leave behind;
- what later work remains, without designing that work early.

Most steps follow the dependency-based order. A clearly bounded step may proceed beside it when its purpose, inputs, boundary, result, and checks are already settled. Do not use shared feature membership as proof that a task is ready. If later integration must decide its behavior or interface, leave it in the plan rather than creating a detailed brief.

The kind of code being built does not decide whether work can start. Detail the step only when its current behavior, boundary, result, and acceptance are meaningful without guessing later design.

## Minimum Content

A useful brief contains only the applicable items:

1. **Goal and result.** State the behavior or capability completed by this step and how it advances the larger plan.
2. **Prerequisites.** Cite the accepted spec sections, earlier results, repo facilities, and assumptions this step relies on.
3. **Requirements.** State the current behavior, inputs, outputs, failure rules, ownership or lifetime, assumptions shared with other parts, and any condition that must remain true for correctness across parts or steps.
4. **Implementation freedom.** Distinguish fixed contracts from private choices such as helpers, control flow, internal structures, naming, and routine repo mechanics.
5. **Edit boundaries.** Name expected source and test areas, frozen artifacts, behavior that must remain unchanged, and changes requiring a new decision.
6. **Acceptance.** State observable checks for relevant normal, boundary, and failure behavior.
7. **Execution boundary.** State whether this step awaits approval, belongs to an approved autonomous outline, or is ready for handoff.

Point to the defining section for shared rules. Restate the concrete obligation needed to make the brief usable on its own, but do not redefine the common rule.

## Repo Fit and Scope

Expected paths focus inspection; they are not an exhaustive file list unless the user explicitly makes them one. Permit small mechanical edits required by observed repo practice, such as registration, exports, build integration, or generated-file updates, and report them with the result.

Pause when necessary work would change accepted behavior, a public interface or shared assumption, responsibility or ownership, acceptance, fixture meaning, or the approved scope. Resolve ordinary private choices from repo evidence without escalating them.

Do not introduce a new abstraction, compatibility layer, asynchronous path, cache, recovery scheme, or other machinery unless the current result or repo requires it. A future scale estimate supports avoiding obvious waste and hard-to-change limits; it does not make future optimization part of this step.

A facade or stub is valid only when the current step completes and verifies a real owning or integration path without pretending deferred behavior exists. State what works now, what remains absent, and how the current claim is checked.

## Acceptance and Commit Shape

State general behavior and the input domain before examples. Examples and illustrative fixtures are representative unless explicitly exhaustive. Acceptance, golden, or normative fixtures have the authority assigned by the spec and repo. Verification covers the relevant cases implied by the requirements rather than only the listed examples.

Make the step small enough that its behavior, edits, and verification can be reviewed together. It may map to one commit, several inseparable commits, or part of a larger approved commit according to repo and user policy. Commit count does not define the plan.

When the workflow requires approval before editing, summarize the goal, prerequisites, fixed contracts, expected edit and test area, acceptance, sensitive boundaries, first action, commit behavior, and unresolved risk. At completion, report the result, checks run, necessary collateral edits, remaining risk, and next action. Update durable records only when a future executor needs changed state or an accepted choice.

## Brief Check

Before using the brief, ask:

- Can an executor understand the current result without rereading unrelated design detail?
- Is the step ready because its own boundary and result are known, not merely because it belongs to the same feature?
- Does it respect real prerequisites without inventing a perfect chain?
- Are contracts clear and private code choices open?
- Can acceptance pass without unfinished future behavior?
- Are approval and frozen boundaries clear?
- Can production code be written without copying the brief's explanatory vocabulary or structure?
