# Evaluation And Synthesis

Subagent outputs are advisory. Evaluate them before using them, retry only for concrete reasons, and synthesize them into one responsible answer.

## Quality gates

Positive signs:

- Answers the requested question directly
- Uses the supplied context instead of generic boilerplate
- Provides evidence, concrete references, or traceable reasoning
- Respects scope and non-goals
- Distinguishes confidence from uncertainty
- Challenges assumptions in the prompt when the artifact does not support them
- Can conclude that no substantive issue exists without manufacturing findings

Failure signals:

- Misunderstands the task or answers a different question
- Ignores key constraints, files, assumptions, or boundaries
- Gives generic review comments without concrete support
- Repeats prompt language without adding analysis
- Accepts the prompt's implied verdict or severity without independent support
- Produces token minor findings because the prompt presupposed a problem
- Sounds confident while showing little evidence
- Conflicts with local facts or peer outputs without explaining why
- Focuses on surface style when the task asked for behavior, risk, or logic

## Retry and escalation

Retry only when there is a concrete reason to expect a better result.

Preferred order:

1. Fix the task statement.
2. Add the missing context or artifact.
3. Retry in a fresh context if independence matters.
4. Increase capability or reasoning depth if the task is genuinely judgment-heavy.
5. Run one more targeted verification step if disagreement remains.
6. Stop and inspect directly if retries are no longer adding information.

If no real subagent path exists, use same-agent verification only as a local check. Do not describe it as an independent review.

## Synthesis

Compare outputs on:

- Scope coverage
- Evidence quality
- Assumptions
- Claimed risks
- Remaining uncertainty
- Independence from prompt framing

Agreement does not guarantee correctness. Conflict does not automatically mean one side is wrong. Identify whether disagreements come from facts, assumptions, or interpretation, keep supported overlap, and preserve unresolved uncertainty when it remains.

Final synthesis checklist:

- State the main conclusion.
- State the evidence basis.
- State what remains uncertain.
- State the recommended next action.
- Make clear that the final judgment belongs to the main agent.
