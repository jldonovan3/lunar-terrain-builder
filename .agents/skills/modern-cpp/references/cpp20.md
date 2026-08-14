# C++20 Guidance

Use C++20 to improve interfaces, contracts, and concurrency, but keep the adoption selective.

The examples below are representative, not exhaustive. The usable feature set is determined by the effective standard, compiler, standard library, project constraints, and local build evidence.

## Language features

- Concepts for constraining templates when the result is clearer diagnostics and intent
- Ranges when they make local transformations clearer instead of more opaque
- Three-way comparison when the type has a natural total or partial ordering
- Designated initializers when aggregate construction becomes easier to read
- `consteval` and immediate functions only when compile-time enforcement is the actual goal
- Coroutines only when the project explicitly wants them and has a mature runtime story

## Library features

- `std::span` for non-owning contiguous views
- `std::jthread` and `std::stop_token` for cancellation-aware thread management
- `std::source_location` for diagnostics and tracing hooks
- `std::format` when available and preferred over ad-hoc formatting
- `std::bit_cast` when representation-preserving conversion is intended
- Calendar, timezone, and chrono formatting facilities when the project already deals with time-heavy logic
- `std::erase` and `std::erase_if` for straightforward container cleanup
- `std::ssize` when signed sizes improve correctness

## Good default candidates

- Start with `std::span`, `std::source_location`, `std::erase_if`, and selective concepts because they often improve contracts with limited churn.
- Use concepts to simplify template contracts, not to build a type-system showcase.
- Use ranges when the pipeline stays readable at the call site and under a debugger.

## Needs extra verification

- Check library support before recommending `std::format`, timezone facilities, or heavier ranges usage.
- Treat `std::format`, chrono formatting or timezone pieces, and heavier ranges facilities as implementation-sensitive even in nominal C++20 projects.
- Keep modules and coroutines opt-in unless the project explicitly wants them and the toolchain story is mature enough.
- Be conservative with concepts in public APIs if they materially complicate downstream diagnostics or compatibility.

## When uncertain

C++20 is a strong target for teams that want better contracts and standard concurrency primitives without jumping straight to the newest language mode. If several modern options are plausible, check authoritative references such as cppreference, feature-test macros, and the active vendor's support notes before picking one.
