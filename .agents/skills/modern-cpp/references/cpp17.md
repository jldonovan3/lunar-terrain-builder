# C++17 Guidance

Treat C++17 as a solid modernization baseline for legacy code and conservative environments.

The examples below are representative, not exhaustive. The usable feature set is determined by the effective standard, compiler, standard library, project constraints, and local build evidence.

## Language features

- Structured bindings for local unpacking with clear names
- `if` and `switch` initializers to keep temporary state scoped
- `if constexpr` to simplify template branching
- Fold expressions when they simplify variadic code instead of hiding it
- Inline variables when they remove header-only boilerplate cleanly
- Class template argument deduction when construction stays obvious

## Library features

- `std::optional` for value-or-no-value contracts
- `std::variant` when a closed set of alternatives is the real model
- `std::string_view` for non-owning read-only string parameters with clear lifetime
- `std::filesystem` when platform file work no longer needs custom wrappers
- `std::byte` when raw storage intent should be explicit
- `std::invoke` and related traits when generic call handling is already part of the design

## Good default candidates

- Prefer upgrades that remove ambiguity and boilerplate with little design churn.
- Consider `std::optional`, `std::string_view`, structured bindings, and `if constexpr` first because they are often local and low-risk.
- Keep `std::variant` visitation readable. If it becomes a metaprogramming exercise, step back.

## Needs extra verification

- Be careful with `std::string_view` lifetime at ownership boundaries.
- Check `std::filesystem` availability and platform behavior before adopting it in portability-sensitive code.
- Avoid clever template machinery just because C++17 makes it easier to write.

## When uncertain

C++17 is often a good first stop for projects moving from older dialects because many upgrades are local and low-risk. If a better feature might exist but the exact fit is unclear, check authoritative references such as cppreference before recommending it.
