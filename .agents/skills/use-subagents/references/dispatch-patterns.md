# Neutral Dispatch Patterns

## Core Standard

A good assignment is concise, self-contained, artifact-centered, and neutral
about the result. Include only information required to answer the question
correctly. Extra context is harmful when it anchors the reviewer or makes the
desired conclusion obvious.

## Minimal Shape

Use the needed subset, not a fixed form:

```text
Task:
Scope or artifacts:
Requirements or constraints:
Expected result:
Completion signal:
```

Add operating conditions, non-goals, or output fields only when they materially
change execution or evaluation.

## Independent Review

Prefer:

```text
Assess <artifact> against <requirements and relevant repo context> without
assuming that it passes or fails. Report only substantive conclusions about
correctness, behavior, design, or maintainability, and support each with
concrete evidence. If no substantive issue is found, state that directly.
Distinguish confirmed findings from uncertainty.
```

Do not write prompts such as:

- confirm that the approach is sound;
- find the remaining issues;
- check whether the suspected bug exists;
- review this mostly complete implementation;
- focus on why option A is safer;
- validate the concerns raised by another reviewer.

Those forms imply a conclusion, expected issue count, or preferred direction.
When a named hypothesis is genuinely the object of investigation, use symmetric
wording:

```text
Assess whether <hypothesis> is supported. Look for evidence that confirms or
falsifies it, and report alternative explanations supported by the artifact.
```

## Artifact Selection

- Pass complete relevant files, diffs, tests, logs, data, or requirements when
  practical.
- Do not pass only the excerpts that support the main agent's concern.
- Remove peer verdicts, review history, severity labels, and persuasive
  summaries unless the task explicitly studies those artifacts.
- Keep authoritative constraints that genuinely determine correctness.
- Do not hide material contrary evidence in the name of brevity.

## Implementation Assignment

State the behavior, scope, acceptance, authoritative repo context, and boundary
conditions. Do not provide speculative classes, private member lists, helper
decomposition, or production-shaped pseudocode unless they are explicit settled
requirements.

Require the implementing agent to inspect the relevant repo instructions and
surrounding code, then derive private structure, naming, and comments in
repo-native form.

## Output Shaping

Constrain output only when it aids evaluation. A compact review may request:

- conclusion;
- evidence;
- substantive findings;
- uncertainty;
- recommended next action.

Do not require a finding count, severity distribution, approval verdict, or
tradeoff matrix unless the task genuinely needs one.
