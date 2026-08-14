# Standard Selection

Establish the target standard before making version-specific recommendations.

## Source order

Use the strongest available evidence in this order:

1. Explicit user instruction
2. Project-declared language standard
3. Build system configuration and compiler flags
4. Toolchain evidence from the active environment
5. Local code patterns only as weak hints

## Working rules

- If the user says "target C++20" or similar, use that as the effective standard unless they later change it.
- If the project clearly declares a standard, align with it instead of guessing from isolated syntax.
- If compiler support and standard-library support differ, plan for the lower effective capability.
- If evidence is missing, conflicting, or too weak to trust, ask the user which standard to target.
- If the user asks for "modern C++" without a standard, do not silently jump to the newest version.

## Support checks

- For library-heavy recommendations, verify more than the language mode when possible.
- Check compiler version, standard-library implementation, and relevant feature-test macros when available.
- Prefer implementation evidence in this order when the recommendation depends on library support:
  1. feature-test macros from the relevant headers, often via `<version>`
  2. vendor implementation status pages for the active standard library or compiler
  3. build evidence or a minimal local probe
- If support is still uncertain, ask, mark the recommendation as tentative, or provide a fallback instead of presenting the feature as ready.
- Prefer build evidence or a minimal probe over assumption when the recommendation depends on newer library facilities.

Useful verification sources include feature-test macros, cppreference feature-test references, libstdc++ status notes, libc++ status notes, MSVC conformance notes, and local compile probes. Use current sources instead of embedding support tables in the skill.

## Toolchain notes

- Language support and standard-library support may advance at different speeds.
- Library-heavy recommendations require both compiler parsing support and the needed library implementation.
- Do not treat `__cplusplus`, `/std:...`, or `-std=...` alone as enough evidence for newer library facilities.
- When the exact implementation status matters, prefer vendor sources such as libstdc++ status pages, libc++ feature-test tables, and MSVC conformance notes over memory.
- C++26 requires extra caution. Treat it as an explicit target, not a default modernization baseline.

## Output rule

Report the chosen standard briefly before proposing version-specific changes. If the standard remains uncertain, ask first.
