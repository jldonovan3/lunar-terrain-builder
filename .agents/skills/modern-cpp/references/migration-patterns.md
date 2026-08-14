# Migration Patterns

Use these patterns to modernize code incrementally.

## Start local

- Prefer a small migration that solves the current problem over a broad style sweep.
- Keep old and new patterns interoperable when that reduces rollout risk.
- Separate semantic changes from representation changes when possible.

## Common migration shapes

### Observer ranges

- Migrate `T* + size` or `begin/end` parameter pairs to `std::span` when the caller already owns the data.
- Do not use `std::span` to hide unclear ownership.

### Optional results

- Migrate sentinel values or separate success flags to `std::optional` when the operation either yields a value or does not.
- Keep error-bearing paths separate from "value absent" paths.

### Value-or-error flows

- Migrate status-plus-output-parameter APIs to `std::expected` when the standard and library support it and the flow becomes clearer.
- Avoid `std::expected` in public interfaces when downstream compatibility is a concern and the ecosystem is not ready.

### String and view boundaries

- Accept `std::string_view` for read-only string-like input when lifetime is clearly bounded by the call.
- Continue to own strings with `std::string` when storage or lifetime must cross the call boundary.

### Public and stable boundaries

- Do not move newer standard-library types into stable or public interfaces unless downstream language and toolchain expectations are known and acceptable.
- Prefer keeping newer facilities internal and adapting at the boundary when consumer compatibility is uncertain.

### Narrow utility upgrades

- Prefer `enum class`, structured bindings, `if` with initializer, `[[nodiscard]]`, and similar local clarity improvements when they reduce ambiguity without changing architecture.

## Escalate instead of patching

When local cleanup repeatedly points to the same design problem, stop proposing isolated syntax upgrades and explain the better abstraction or boundary.
