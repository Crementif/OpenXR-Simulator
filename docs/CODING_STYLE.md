# Coding style and verification

Use C++20 with Allman braces (lambda bodies open on the lambda's line), four-column tabs, a blank line between function definitions, PascalCase types and functions, camelCase locals, `m_` members and `s_` statics. Group a class's declarations by purpose, public interface first, and define them in the `.cpp` in the same order. Prefer `while (true)` over `for (;;)`. Follow `.clang-format`, `.clang-tidy` and `.editorconfig`; preserve vendored and generated code. `scripts/tidy.py` formats and tidies the sources; `scripts/tidy.py --check --changed` checks a change before committing. Choose clear names and direct control flow. Add comments sparingly to explain constraints or behavior that the code cannot make clear; do not narrate changes or restate code.

Prefer values, standard containers and RAII. Use shared ownership only for shared lifetimes. Keep platform APIs in adapters and check errors, widths, encoding and time units at boundaries. Use `std::filesystem::path` and RAII streams for ordinary file I/O, preserving Unicode paths and read, write and close errors. Own and join workers, retain deferred inputs, and handle worker exceptions. Do not hold global locks across pacing, GPU waits, IPC or file I/O. Preserve GPU completion and resource lifetimes.

Separate ImGui UI, application services and GPU rendering. Reserve the root of `src/app` for entry points and composition; put nontrivial implementations in `.cpp` files. Avoid forwarding-only classes, speculative scaffolding and monoliths hidden behind PImpl. Centralize common stable includes in `src/Pch.h` and include it explicitly in implementation files so non-PCH builds work. Keep specialized and platform headers local, public headers self-contained, and PCH artifacts compatible with their targets. Do not use anonymous, unnamed or empty namespaces.

Finish a coherent change, inspect its diff and affected callers, then run focused checks. Add regression tests for meaningful bugs and contracts, not edits that merely repeat the implementation. Build and run the relevant tests with the configured CMake preset, for example:

```sh
cmake --build --preset debug
ctest --preset debug --output-on-failure
```

Verify observable behavior and UI usability when affected. Exercise graphics changes on the relevant native APIs and hardware, including synchronization and resource lifetimes; missing hardware or skipped tests are not passes. Cross-compilation checks source and link compatibility, not native execution. Reuse passing results and rerun affected checks when changes or failures warrant it.
