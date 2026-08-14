# Project Profile

Establish the project constraints that shape safe modern C++ adoption.

## Constraint signals

- Hosted or freestanding environment
- Public headers, ABI stability, and downstream consumer standards
- Exception, RTTI, allocation, and threading policies
- Debugging, sanitizer, and observability expectations
- Performance-sensitive paths, real-time constraints, and binary-size pressure
- C and platform API boundaries

## Working rule

Treat these constraints as implementation requirements. Choose modern C++ forms that fit the project's runtime model, compatibility surface, and maintenance expectations.
