# Questioning Rules

Ask less, but ask better.

## Good question targets

- Intended deliverable
- Execution owner: human or agent
- Whether the user wants implementation detail or mainly the final result
- Execution context when it changes the plan or prompt shape
- Execution boundaries when they change what the agent may safely do
- Whether there is a real repository, file set, or workspace to inspect now
- Scope boundaries and non-goals
- Constraints that change the plan
- Missing local facts the user is best positioned to supply

## Good question style

- Keep questions short and easy to answer.
- Prefer one to three questions at a time.
- Ask about decisions, not essays.
- Use the current understanding summary to show what is already understood.
- Let the agent own obvious inspection and triage choices.
- When the user appears result-oriented, ask only for inputs, acceptance
  criteria, or execution boundaries that materially affect delivery.

## Avoid

- Asking the user to restate the whole task
- Turning clarification into a template or form
- Asking about details that do not affect the plan
- Asking whether the user wants process detail when that preference is already obvious
- Reconfirming obvious facts from the current request
- Repeatedly asking whether to inspect local context when the agent can determine that itself
- Asking speculative future-state questions unless they materially change the current scope
