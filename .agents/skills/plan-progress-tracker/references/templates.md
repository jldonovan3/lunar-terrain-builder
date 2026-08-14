# Optional Workpack Shapes

Use only the sections that current content requires. These are compact starting
shapes, not mandatory outlines. Omit empty sections and whole files.

## Entry

```md
# <Workpack Name>

Start: `<current document or task>`

## Documents

- `<path>` — <authoritative purpose>
```

Use an entry file only when a multi-file workpack needs routing.

## Overview

```md
# <Workpack Name>

## Goal

<Durable outcome>

## Scope and Constraints

- <Only constraints that affect this work>

## Acceptance

- <Observable completion condition>

## Execution Path

1. <High-level step or milestone>
```

Omit the overview when an existing spec or repo document already owns this
information.

## Plan

```md
# Plan

| ID | Status | Task | Acceptance | Links |
|---:|:------:|:-----|:-----------|:------|
| T001 | TODO | <durable task> | <pass condition> | <repo-relative links> |
```

Use stable IDs only when later documents or sessions need to reference tasks.
For a short linear plan, a simple checklist is sufficient.

## Status

```md
# Status

- Current: <task, milestone, or state>
- Blocked: <material blocker or None>
- Next: <single next action>
- Links: <essential repo-relative references>
```

Do not add activity logs, completed micro-steps, review summaries, or temporary
investigation notes.

## Decision

```md
## <Decision ID or concise title>

- Decision: <current effective choice>
- Basis: <technical evidence needed for future judgment>
- Affects: <requirements, tasks, or downstream work>
- Supersedes: <earlier decision, only when relevant>
```

Use a decision record only when corrected canonical text alone would not retain
information future work still needs.
