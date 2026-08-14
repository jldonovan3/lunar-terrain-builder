# Adoption Principles

Favor modern CMake patterns that make the build easier to understand and evolve.

## Default goals

- Prefer target-based configuration over directory-scoped or global configuration.
- Keep usage requirements explicit through target APIs.
- Keep project layout and helper modules consistent by responsibility.
- Fail early on invalid configuration combinations.
- Preserve portability and consumer compatibility where they matter.

## Good default candidates

- `target_link_libraries`, `target_include_directories`, `target_compile_definitions`, and `target_compile_features`
- Presets when the project has multiple recurring workflows worth naming explicitly
- Small helper modules for platform, compiler, install, or validation concerns
- Imported targets from `find_package` instead of raw variables where available
- `FILE_SET` for public headers or generated headers when there is a real boundary and the effective CMake version supports it
- `CTest` integration when testing is part of the project today

## Keep it simple

- Start with the smallest clear target graph that matches the real requirements.
- Prefer obvious target names and dependency flow over clever indirection.
- Do not multiply targets or helper modules just to look more modern.
- Keep compiler options conservative by default, especially for small projects.
- On MSVC, UTF-8 compatibility is a reasonable default before heavier compiler tuning.

## When uncertain

If a modern feature seems promising but support or tradeoffs are unclear, verify it against the official CMake documentation before recommending it as the default path.
