# Plan Loop Pattern

Treat this skill as a compact planning loop rather than a single rewrite pass.

## Default loop

1. Inspect available local context if it is actually relevant and available.
2. Summarize current understanding.
3. Identify which uncertainties would actually change the plan.
4. Ask a few high-value questions.
5. Research external facts when they matter.
6. Mark what is confirmed, inferred, and still unknown.
7. Converge on a right-sized brief.

## Stop conditions

- Stop asking questions once remaining uncertainty no longer changes execution.
- Skip the question round entirely when the request is already clear enough to produce a solid brief.
- Confirm the intended executor early when it materially changes the output shape.
- Do not treat the current session workspace or working directory as
  authoritative unless the user made it relevant.
- Keep inspection incremental. Prefer a narrow check that matches the request over broad recursive exploration.
- If a critical gap remains and neither browsing nor local inspection can resolve it, ask the user directly instead of pushing ahead on guesswork.

## Output posture

- Prefer a provisional brief with explicit assumptions over an overbuilt plan with false certainty.
- Prefer a final brief over continued questioning once the plan is stable enough to execute.
