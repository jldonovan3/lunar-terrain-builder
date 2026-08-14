# Package and Install

Treat install and package support as explicit product requirements, not default ceremony.

## Add install support when it is needed now

- Add install rules, exported targets, and package config files when the project will actually be installed or consumed externally.
- Do not add the full package surface for a practice project or a local-only tool unless the user asks for it.

## Keep future expansion easy

- Keep public headers, generated headers, and target boundaries clear.
- Prefer layouts that can grow into install support without moving everything later.
- `FILE_SET` is a good candidate when public or generated headers need to stay aligned between build and install trees and the effective CMake baseline supports it.

## Build-tree vs install-tree

- Keep helper targets, private implementation details, and exported targets intentionally separated.
- Avoid leaking private compile definitions or internal-only dependencies into installed interfaces.

## Dependency boundaries

- Prefer imported targets for public dependency propagation.
- Prefer the project's existing dependency acquisition model unless there is a clear reason to change it.
- If the project already clearly uses `find_package`, vendoring, submodules, `FetchContent`, or another dependency path, inherit that model by default unless the user asks to revisit it.
- For reusable libraries, lean toward externally managed dependencies before vendoring more code into the build.
- Be deliberate about what becomes part of the installed consumer contract.

## When uncertain

If install or export behavior depends on CMake version details or package config subtleties, verify against the official CMake documentation before locking in the design.

Prefer the official Importing and Exporting Guide as the first reference point:

- [Importing and Exporting Guide](https://cmake.org/cmake/help/latest/guide/importing-exporting/index.html)

When version-sensitive install or export behavior matters, prefer the guide and command docs for the project's effective CMake version rather than assuming `latest` semantics.

When a mature open-source library already solves a similar install or package problem cleanly, it is reasonable to inspect that public CMake layout for patterns after the official guide.
