---
name: use-subagents
description: >
  Decide when subagents materially improve a task, dispatch concise neutral
  assignments, preserve independent review, evaluate outputs, and synthesize
  evidence without outsourcing judgment. Use for independent review, design or
  algorithm critique, debugging hypotheses, research analysis, verification,
  or bounded parallel work.
---

# Use Subagents

## Purpose

Use subagents only when independent information, coverage, challenge, or
verification is worth the coordination cost. Keep the main agent responsible
for task framing, evidence quality, conflict resolution, and the final result.

This skill does not require custom agent roles or a fixed orchestration system.
When true independent contexts are unavailable, use local decomposition but do
not describe it as independent review.

## Hard Rules

1. **Delegation must add value.** Use no subagent for routine or tightly coupled
   work when another context is unlikely to add information, coverage, or
   verification.
2. **Assignments must be concise, clear, and complete.** State the exact
   question, relevant scope, necessary authoritative artifacts, material
   constraints, and expected result. Include only context needed to perform the
   task correctly.
3. **Independent review must be conclusion-neutral.** Do not directly or
   indirectly signal that an artifact is correct, flawed, preferred, nearly
   complete, or expected to produce a particular finding. Do not ask the
   reviewer to confirm, approve, validate a favored conclusion, find remaining
   issues, or focus on suspected defects unless that exact hypothesis is the
   object being tested.
4. **Pass evidence, not persuasion.** Prefer raw code, diffs, documents, logs,
   data, requirements, and neutral questions. Omit the main agent's verdict,
   peer conclusions, review history, rejected alternatives, emotional wording,
   severity cues, and unnecessary conversation context.
5. **Do not anchor through selection or framing.** Provide the complete relevant
   artifact and symmetric evaluation criteria when practical. Do not expose only
   evidence supporting one interpretation, label one option as the baseline, or
   frame critique as cleanup after an assumed-correct design.
6. **Implementation delegation follows settled behavior.** Delegate code only
   after the relevant behavior, scope, acceptance, and integration boundary are
   clear. Pass requirements rather than speculative private structure, class
   declarations, helper layouts, or spec-only terminology. The implementing
   agent must inspect repo conventions and derive private code independently.
7. **Delegated slices need independent value.** A subtask must produce an
   independently useful result or verification signal. Do not split helper-level
   work merely to create parallelism.
8. **Outputs are advisory.** Evaluate scope, evidence, assumptions, local
   context, uncertainty, and contradictions before relying on any result.
9. **Synthesize by evidence, not agreement.** Multiple matching opinions do not
   establish correctness. Resolve conflicts from artifacts and reasoning, and
   preserve genuine uncertainty.

## Workflow

### 1. Decide whether to delegate

Classify the task as execution, exploration, independent review, design
critique, verification, debugging, or synthesis. Delegate only when another
context can contribute something distinct.

Use:

- no subagent for routine, sequential, or low-risk work;
- one subagent for one bounded analysis, check, or separable support task;
- two independent subagents only when comparison or contradiction detection is
  worth the added cost.

Read `references/task-selection.md` when the choice is unclear.

### 2. Build a neutral assignment

Start from the artifact and question, not the desired answer. Include the
minimum needed subset of:

- exact task or question;
- task type, when it changes how the result should be judged;
- in-scope artifacts or paths;
- material requirements and constraints;
- relevant operating conditions;
- expected output and completion signal.

For independent review, ask for an assessment against the artifact,
requirements, and repo evidence. Do not disclose the main agent's conclusion or
suspected issue. If a hypothesis must be tested, state it as a falsifiable
hypothesis and ask for evidence both for and against it.

Read `references/dispatch-patterns.md` before drafting a review, critique, or
implementation assignment.

### 3. Preserve independence

Use a fresh context for independent review or contradiction checking when
possible. Supply raw artifacts rather than summaries that embed conclusions.
When multiple independent reviews are used, do not show one reviewer's output to
another before both finish.

### 4. Evaluate the result

Check whether the subagent:

- answered the actual question;
- inspected the supplied evidence;
- respected scope and constraints;
- distinguished fact, inference, and uncertainty;
- produced concrete, traceable reasoning;
- resisted prompt assumptions rather than merely echoing them.

Retry only when a better task statement, missing artifact, fresh context, or
stronger capability has a concrete chance of improving the result.

Read `references/evaluation-and-synthesis.md` when evaluating, retrying, or
combining outputs.

### 5. Synthesize responsibly

The main agent makes the final judgment. Compare evidence quality, scope,
assumptions, and uncertainty. State confirmed conclusions, unresolved issues,
and the next action without treating vote count as proof.

## Self-check

Before dispatching or relying on a subagent, verify:

- delegation has a concrete information or verification benefit;
- the assignment is short enough to avoid irrelevant anchoring;
- no wording implies the expected conclusion or acceptable severity;
- raw relevant artifacts are preferred over persuasive summaries;
- evaluation criteria apply symmetrically to all plausible conclusions;
- implementation prompts contain settled requirements, not a private code
  skeleton;
- the result was checked against evidence rather than accepted by authority or
  agreement.
