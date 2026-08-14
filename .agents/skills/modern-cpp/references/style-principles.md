# Style Principles

Apply these semantic style defaults across projects unless the repository defines stronger local rules.

## Core defaults

- Prefer simple interfaces with explicit data flow.
- Prefer value semantics for local ownership and regular types.
- Make borrowing, ownership, and lifetime visible in types and function signatures.
- Use RAII to make cleanup automatic and exception-safe.
- Prefer standard-library facilities over custom wrappers when they carry the right meaning.

## Abstraction rules

- Introduce abstraction to capture a stable concept, constraint, or invariant.
- Require a concrete benefit in correctness, ownership, lifetime, dependency
  direction, reuse, or caller clarity before adding a type, interface, or
  helper. Count construction, conversion, wiring, diagnostics, and navigation
  as costs.
- Keep closely related state and behavior together when they share ownership,
  lifetime, invariants, and reasons to change.
- Do not introduce abstraction only to look modern.
- Keep templates and concepts narrow and readable.
- Prefer direct code over generic machinery when the generic version does not materially improve reuse or safety.

## Cost visibility

- Avoid APIs that hide allocation, ownership transfer, or expensive work.
- Be careful with lazy views, proxy types, and deeply composed pipelines in code that must be debugged often.
- Treat compile-time cost as a real cost.

## Contracts

- Use stronger types when they prevent a realistic misuse, preserve an
  important invariant, or establish a meaningful boundary. Semantic distinction
  alone does not require a wrapper type.
- Use `const`, `constexpr`, and `noexcept` when they are correct and helpful, not by reflex.
- Keep error handling explicit. Choose mechanisms that match the contract and the calling style.

## Public documentation

- Describe APIs from the caller's perspective and make their intended use
  understandable without internal design context.
- Document copy, ownership, lifetime, concurrency, and failure details only when
  they affect correct use.
- Keep internal architecture classifications and planning vocabulary out of
  public names and comments unless they are established domain language.

## Change posture

- Prefer focused improvements that leave surrounding stable code alone.
- Suggest follow-up migrations separately when they require broader coordination.
