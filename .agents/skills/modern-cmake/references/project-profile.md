# Project Profile

Establish the build context before recommending structure or newer CMake features.

## First questions

- Is this a new project or a migration of an existing one?
- Is this a practice project, an internal tool, an application, or a reusable library?
- Does it need tests, install rules, exported targets, package config files, IDE support, or external consumers now?
- Does it need to support multiple generators, multiple platforms, or cross-compiling?

## Default posture by project type

### Practice and small local projects

- Keep the build minimal and easy to read.
- Prefer a small target-based layout.
- Add presets only when they solve repeated entry-point needs the user actually has.
- Do not add install or package machinery unless the user asks for it.
- Avoid complex compiler-flag tuning by default. On MSVC, UTF-8 compatibility is a reasonable default consideration.

### General engineering projects

- Use presets for repeatable entry points when the project has multiple common build modes and the user wants a stable configure or build interface.
- Organize helper modules by responsibility instead of collecting unrelated logic in one file.
- Decide explicitly whether install, export, and test support belong in the first version.
- Keep target and directory organization aligned with the actual dependency structure rather than an abstract ideal template.

### Libraries and externally consumed projects

- Confirm install, export, namespace, package config, public headers, and versioning needs early.
- Keep build-tree and install-tree behavior intentionally separated.
- Be more careful with dependency propagation and public compile requirements.

## Version and generator checks

- Check the minimum CMake version before recommending newer features such as presets refinements, `FILE_SET`, or newer install behavior.
- Do not recommend a feature only because the local CMake is new enough. Prefer the project's declared baseline and the user's acceptable minimum version.
- If a recommendation would effectively raise the project's practical CMake floor, say so explicitly instead of treating the feature as free.
- Check whether the project uses a single-config or multi-config generator.
- Check whether toolchain files, cross-compiling, or IDE integration impose constraints on the design.
- Check official CMake documentation when a recommendation depends on version-floor claims, preset schema details, generator-specific behavior, cross-compiling semantics, install or export behavior, or policy changes.
- For version-sensitive behavior, prefer the specific command, feature, policy, preset, or guide page over generic memory about "modern CMake."
- When semantics differ by CMake release, prefer documentation that matches the project's effective CMake version instead of assuming `latest` behavior applies.

## When uncertain

If version or generator behavior matters and the exact support level is unclear, verify against the official CMake documentation before recommending the feature.
