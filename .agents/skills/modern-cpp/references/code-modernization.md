# Code Modernization

Use this pass after establishing the project profile and effective standard. Code modernization means source-level changes that keep behavior and contracts stable while making C++ code clearer, safer, and more idiomatic within the effective standard.

## Scope

- Work at expression, statement, function, small type, or private-helper scope.
- Treat public interfaces, ABI-sensitive boundaries, and observable behavior as stable. Apply contract changes when the task explicitly includes them.
- Prefer one clear reason for each patch group: intent, lifetime, safety, cost, or readability.
- When the same issue recurs across modules, implement a contained slice and record the broader migration direction.
- Treat the review lenses below as required inputs. Add context-specific improvements when the surrounding code, domain model, or performance profile points to a better C++ expression.

## Review lenses

Use these lenses together as a compact code-level review checklist.

### Modern expression

- Prefer target-standard forms that state intent: `nullptr`, `override`, `using`, default member initializers, defaulted or deleted special members, structured bindings, and `[[nodiscard]]`.
- Prefer RAII, standard ownership types, and standard construction helpers when they make ownership or construction explicit.
- Use `constexpr`, `noexcept`, and stronger types when they express a real contract.

### Correctness and lifetime

- Make ownership, borrowing, moved-from state, optional presence, narrowing conversions, and iterator validity visible in types or nearby control flow.
- Use `std::span` and `std::string_view` when borrowing and lifetime are already clear at the call boundary.
- Prefer APIs that encode valid states and reduce caller convention.

### Local efficiency

- Reduce accidental copies in range-for loops, parameter passing, temporary-heavy expressions, strings, and containers.
- Prefer standard-library operations with clear complexity, allocation, and traversal behavior.
- Use move semantics where ownership transfer is intended and visible.

### Readability

- Prefer direct control flow, clear container queries, and simple boolean expressions.
- Use `auto` when it removes repetition while preserving important type meaning at the call site.
- Prefer named algorithms when the algorithm name states the operation better than loop mechanics.

## Algorithms and ranges

Prefer named algorithms when the loop is doing a standard sequence operation. Prefer ranges when the target is C++20 or newer, support is reliable, and the range form reads better at the call site.

Common fits:

- search and matching: `std::find`, `std::find_if`, `std::any_of`, `std::all_of`, `std::count_if`
- selection and transformation: `std::copy_if`, `std::transform`
- removal and cleanup: `std::erase`, `std::erase_if`
- ordering and grouping: `std::sort`, `std::stable_sort`, `std::partition`
- aggregation: `std::accumulate` or `std::reduce` when the operation stays obvious

Prefer range-for when the body is the clearest expression of a small domain action. Reserve `std::for_each` for cases where the callback form is the clearest fit. Keep pipelines short enough that evaluation order, lifetimes, and debugging remain understandable.

## Response rule

When applying or recommending code modernization, state the target standard briefly, name the practical payoff, and call out the main lifetime, compatibility, readability, or local-efficiency consideration.
