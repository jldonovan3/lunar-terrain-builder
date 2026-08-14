# Workpack Layout and Ownership

## Select Roles, Not a Fixed Layout

Choose documents by the information that must persist. A workpack may contain
one file or several. Do not create a file whose content would be empty,
duplicated, speculative, or recoverable cheaply from the repo.

Common roles:

- `INDEX.md`: optional entry point for a multi-file workpack; links to the
  current start location and authoritative documents.
- `OVERVIEW.md`: optional durable goal, scope, constraints, acceptance, and
  high-level execution path when no existing spec already owns them.
- `PLAN.md`: optional shared inventory of durable tasks or milestones.
- `STATUS.md`: optional current position, blockers, next action, and essential
  links.
- `DECISIONS.md`: optional material choices whose history or rationale remains
  necessary for future judgment.
- topic files: optional substantial areas that need independent maintenance or
  handoff.

Examples of valid shapes:

```text
<workpack-root>/
  PLAN.md
```

```text
<workpack-root>/
  PLAN.md
  STATUS.md
```

```text
<workpack-root>/
  INDEX.md
  OVERVIEW.md
  PLAN.md
  STATUS.md
  DECISIONS.md
  topics/
    <topic>.md
```

The last shape is appropriate only when every file has distinct durable
ownership.

## Ownership Rules

- Keep each requirement, task, current state, and decision authoritative in one
  place.
- Link to an existing spec or repo document instead of restating its content.
- Keep task definitions in one plan. Status points to the active task or
  milestone rather than repeating the backlog.
- Keep status current and compact. Remove superseded transient notes when
  maintaining the file.
- Correct stable text directly when the corrected result is sufficient for
  future work.
- Retain decision history only when the earlier choice or its supersession still
  matters for auditability, compatibility, or downstream judgment.

## Topic File Gate

Create a topic file only when the topic has substantial independently maintained
content or a separate handoff path. A distinct logical responsibility, task,
class, subsystem name, or phase does not by itself justify another document.

Topic documents describe durable requirements or design information. They do
not prescribe a one-to-one production module, type, interface, or file layout
unless that boundary is already an explicit requirement.

## Existing Workpacks

Preserve an established useful layout during ordinary updates. Do not create
missing files only because a template names them. When reconciliation or
explicit cleanup is requested, collapse empty or duplicative files after moving
their remaining authoritative content to the appropriate owner.

## Portable References

Prefer repo-relative paths and stable URLs. Use a machine-specific absolute path
only when the work cannot be located unambiguously without it or the path is
itself part of the external requirement.
