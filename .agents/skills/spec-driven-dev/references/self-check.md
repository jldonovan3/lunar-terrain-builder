# Self-Check

## Purpose

Review a spec against the confirmed task, repo evidence, whole document, and current design level. Limit findings to concrete mismatches or text that permits materially different delivered behavior, architecture, work order, verification, scope, acceptance, or safe continuation. Template completeness, sentence-level perfection, and lower-level design not yet reached do not meet this threshold.

Read the document as a whole before reviewing individual sections. Report a locally correct sentence when it harms the whole through repetition, contradiction, unnecessary restriction, or distraction; otherwise do not reward or criticize prose in isolation.

Each review round judges the latest document against the confirmed task, repo evidence, and current design level. It does not need to produce a new finding. Do not reopen a confirmed choice unless the current text conflicts with it or new evidence changes its effect. Treat a different valid preference, optional enhancement, wording polish, hypothetical future concern, or detail intentionally left for a later level as an issue only when it creates a concrete execution risk in the current spec.

## Core Review

Use these questions for every spec. They are a risk scan, not required headings.

1. **Purpose:** Does the document guide a concrete development effort rather than teach the domain, tour existing code, or serve as general architecture documentation?
2. **Repo basis:** Does the design follow inspected repo instructions, entry points, facilities, code, tests, public surfaces, fixtures, and conventions? Are conclusions marked provisional when inspection was unavailable?
3. **Goal:** Is the intended result and acceptance clear before local parts and tasks?
4. **Whole flow:** Can a reader understand how one run moves from entry or input to result, including important branches and changes in data or state, identity, ownership, and lifetime?
5. **Plan:** Is the recommended order based mainly on what must exist or be known first? Are related tasks grouped without forcing every neighboring task into a false consumer chain? Is work kept at outline level when later design must determine it?
6. **Right depth:** Can compact work proceed directly, while larger work uses only the middle levels needed for understanding, review, assignment, or resumption? Did the agent recommend and justify the split?
7. **Contracts and freedom:** Are behavior, acceptance, interfaces, shared assumptions, responsibility, ownership, lifetime, and conditions that must remain true across parts explicit when different readings change the result? Are private helpers, control flow, data structures, and ordinary repo choices left open?
8. **One definition:** Is every shared rule defined in one place, with local obligations restated only where a standalone brief needs them?
9. **Plain writing:** Are this skill's rules, applicable working-context instructions, and repo rules followed throughout the work rather than reproduced as spec content? Does each term, symbol, caveat, prohibition, format, and rationale preserve a real decision? Can low-value wording, formalism, repeated background, or decorative structure be removed?
10. **Authority and revision integrity:** Were important choices confirmed before being written or implemented? Does the main spec state one coherent current design, with the revision preserving unaffected meaning and records kept only when later work needs them?

## Planning Risks

Check the following when the spec contains more than a direct implementation list:

- Each stage or group has one clear goal or responsibility.
- The order distinguishes a real prerequisite from a merely convenient sequence.
- A task placed beside the main order already has a clear purpose, boundary, result, and check. Group membership alone is not evidence that it should be implemented now.
- A task's name or code category is not being used as proof that it is ready. Its current purpose, boundary, result, and acceptance are knowable without guessing later design.
- Earlier work does not claim behavior that only unfinished later work can make true.
- Future stages state enough to show feasibility and relationships, without premature commit plans or private design.
- Extra planning levels have an explicit benefit. One or two middle levels should handle most large tasks, but this is a judgment, not a fixed limit.

## Design Risks

Check the following when applicable:

- New development is designed as the requested target system, not as an imagined migration with adapters, compatibility layers, or replacement steps.
- Compatibility follows actual released or installed surfaces, downstream users, persisted data, repo policy, or explicit commitments. New or unreleased work is not preserved merely because code exists; an internal refactor preserves its real external contract and converges on one implementation; an intentional external change has a discussed transition and removal condition when migration support is needed.
- Ideas borrowed from mature projects solve a current problem and fit this repo, scale, and team. Their organizational model is not copied merely because it is respected.
- Existing facilities are reused when suitable, and new abstractions have a current responsibility or contract.
- A future scale estimate leads only to sensible choices that avoid obvious waste or dead ends. Large-scale processing, concurrency, caching, recovery, or similar machinery has a current measurable requirement or observed need.
- Shared behavior is placed with the code that owns its rules and lifetime.
- Public behavior and cross-part assumptions are not hidden in a component-specific section.
- Negative statements supplement a positive model rather than replace it.

## Execution Risks

Check these for a current implementation step or brief:

- The current result, prerequisites, expected edit area, acceptance, and verification are clear.
- Its acceptance does not depend on unfinished future behavior.
- Expected files are useful anchors rather than an unjustified exhaustive edit list.
- Frozen or approval-sensitive artifacts are identified.
- Discussion or approval of an outline has not been treated as approval of unspecified implementation.
- The implementation may choose repo-native private structure without copying the spec's explanatory terms or layout.
- Gated execution waits for the current scope; autonomous execution has explicit authorization and a confirmed overall outline.

## Acceptance and Records

Check that general behavior and input scope precede examples, and that illustrative cases are not mistaken for an exhaustive domain unless declared so. Fixture authority should come from the spec and repo rather than a blanket assumption. Verification should cover relevant normal, boundary, and failure behavior.

If a frozen fixture conflicts with an accepted requirement, the spec must call for a decision rather than silently changing the fixture or coding to the contradiction.

Progress should record only state a future executor needs: current work, completed milestone, blocker, verification state, next action, or return point. A decision record should exist only for an accepted choice whose context is still needed after the main spec is corrected. Conversation history, review chatter, implementation nits, and compatibility trivia do not belong there.

## Reporting Findings

Report only substantive issues, ordered by severity. A substantive issue can lead to wrong or materially divergent behavior, violate an accepted contract, introduce unjustified architecture or scope, follow the wrong work order, miss acceptance, or make execution or continuation unsafe. For each issue, name the affected file and location, explain that consequence, and state the smallest useful correction. Do not pad a review with preferences or nits. When no issue meets this threshold, report that directly and stop unless the user asked for strengths or detailed coverage.

For a Chinese self-check-only request with no requested format, output exactly `未发现实质问题` when no substantive issue is found.
