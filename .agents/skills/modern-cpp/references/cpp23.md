# C++23 Guidance

Use C++23 for focused improvements after confirming real compiler and standard-library support.

The examples below are representative, not exhaustive. The usable feature set is determined by the effective standard, compiler, standard library, project constraints, and local build evidence.

## Language features

- Explicit object parameters only when they make a member-like API materially clearer
- Expanded constexpr support when it simplifies implementation or testing
- `if consteval` when compile-time and runtime paths genuinely differ
- Multidimensional subscript support only when the type and compiler support story are already clear

## Library features

- `std::to_underlying` for explicit enum-to-integer conversion
- `std::expected` for value-or-error flows where the calling code becomes clearer
- Monadic operations for `std::optional` and `std::expected` when they simplify local composition without hiding control flow
- `std::mdspan` for multidimensional views only when the toolchain and problem domain make it worthwhile
- `std::print` and `std::println` only when library support is confirmed and simple console output is the real need
- Ranges quality-of-life additions when they simplify an already good pipeline instead of just changing style
- `std::move_only_function` when move-only call wrappers are genuinely needed

## Good default candidates

- Treat C++23 as an incremental upgrade, not a reason to rewrite C++20 code wholesale.
- Prefer features with a clear payoff in local code first.
- `std::expected`, `std::to_underlying`, and selective constexpr improvements are often the first candidates worth checking.

## Needs extra verification

- Verify library support carefully before proposing library-heavy changes.
- Be careful with `std::expected`, `std::mdspan`, `std::print`, and newer ranges facilities because support and ecosystem readiness can lag the language mode.
- Keep monadic chaining readable; if it obscures control flow, prefer the direct form.

## When uncertain

C++23 works best as a selective quality-of-life upgrade on top of already healthy C++20-era code. If a promising newer facility comes to mind but the exact semantics or support level are unclear, check authoritative references such as cppreference, feature-test macros, and vendor implementation notes before recommending it.
