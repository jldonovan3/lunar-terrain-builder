# Adoption Principles

Favor changes that improve the codebase, not changes that only refresh syntax.

## Default goals

- Improve correctness and resource safety.
- Clarify interfaces, contracts, and ownership.
- Reduce accidental complexity.
- Preserve readability, diagnostics, and debuggability.
- Keep compatibility and maintenance costs under control.

## Default decision checklist

Prefer a newer feature when most of these are true:

- It makes intent or contracts clearer.
- It reduces a real class of bugs.
- It removes boilerplate without obscuring behavior.
- It is available in the effective language standard and standard library.
- It keeps diagnostics and debugging reasonable for the team.

Prefer not to adopt when most of these are true:

- The gain is mostly stylistic.
- The change would expand scope far beyond the local problem.
- The feature would make public interfaces harder to support.
- The debugging or compile-time cost would rise materially.
- The same result is already expressed clearly with simpler code.

## High-value patterns

- Replace custom ownership protocols with RAII and standard ownership types.
- Replace raw pointer plus length pairs with `std::span` when the lifetime is already managed elsewhere.
- Replace ad-hoc optional state with `std::optional` when the absence of a value is part of the contract.
- Replace value-or-error conventions with `std::expected` when the target standard and library support it.
- Replace ambiguous integral flags with stronger types when that clarifies the contract.

## Conservative zones

Stay more conservative in these areas:

- Public APIs and long-lived extension points
- ABI-sensitive libraries
- Low-level performance-critical kernels
- Compile-time-heavy template infrastructure
- Code that many contributors must debug quickly
