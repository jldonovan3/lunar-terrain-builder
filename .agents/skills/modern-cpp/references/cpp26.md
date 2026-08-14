# C++26 Guidance

Treat C++26 as an explicit, carefully verified, and still-experimental target.

## Default posture

- Do not assume C++26 support just because the compiler advertises partial language support.
- Verify both compiler support and standard-library support before recommending any C++26-specific facility.
- Prefer isolated adoption with a small blast radius.
- Keep a fallback plan when proposing C++26 features in shared or long-lived code.

The examples below are representative, not exhaustive. The usable feature set is determined by the effective standard, compiler, standard library, project constraints, and local build evidence.

## Good candidates

- Small self-contained library improvements with obvious local benefit
- Narrow language features that simplify a specific implementation without changing broad interfaces

## High-risk candidates

- Features that alter public APIs or ABI-sensitive boundaries
- Features that significantly raise compiler, debugger, or build-system requirements
- Features that would force mixed-standard consumers into an upgrade immediately

## Migration note

When a user explicitly targets C++26, state the named feature, support assumptions, and fallback plan. Do not present C++26 as the default answer for general modernization.
