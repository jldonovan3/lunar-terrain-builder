# Migration Patterns

Use these patterns to modernize existing CMake code incrementally.

## Start local

- Prefer a small migration that fixes the current problem over a broad rewrite.
- Keep existing build entry points working while moving new logic toward target-based structure.
- Separate structural cleanup from behavior changes when possible.

## Common migration shapes

### Global settings to target settings

- Replace directory-scoped include paths, definitions, and flags with target-based APIs first.
- Move shared behavior into small helper targets or focused helper modules instead of repeating settings everywhere.

### Legacy entry points to presets

- Add presets when the project has repeatable configure and build commands worth standardizing.
- Keep presets aligned with real workflows such as debug, release, sanitizers, or platform-specific development.
- Do not force many preset combinations into a project whose workflow is still simple.

### Source and header organization

- Prefer explicit source lists for core code.
- Consider `FILE_SET` for public headers or generated headers when the version baseline supports it and the boundary is real.
- Do not force `FILE_SET` into a tiny project that gains nothing from it.
- Do not force a fixed directory template such as an explicit `include/` tree unless the project needs that boundary today.

### New project initialization

- Confirm whether install, export, and package config are needed now.
- If not, keep the first version simple and readable.
- Still organize targets, include layout, and generated files so future install support does not require a redesign.
- When evolving an existing project, keep target naming and directory conventions aligned with the current project unless the user asked for a broader reorganization.

## When uncertain

If a migration path depends on newer CMake behavior or subtle generator semantics, verify against the official CMake documentation before prescribing the change.
