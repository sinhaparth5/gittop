# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Repository state

`gittop` is a planned btop-inspired terminal dashboard for Git — local repo state offline, plus GitHub/GitLab CI and PR panels. **No code exists yet.** The only tracked files are `README.md`, `LICENSE`, `.gitignore`, `progress.md`, and this file.

**Read `progress.md` first.** It holds the locked stack decisions, target directory layout, the phased roadmap with per-task checkboxes, known risks, and the work log. Keep it updated as work lands — it is the source of truth for project state, and this file should be re-generated with real architecture notes once there is code to describe.

## Established conventions

- **License: GPL-3.0.** New source files should carry a GPL-3.0 header consistent with whatever the first real sources establish.
- **Build system: CMake.** `.gitignore` is the standard CMake + CLion ignore set (`CMakeCache.txt`, `CMakeFiles`, `_deps`, `compile_commands.json`, `CTestTestfile.cmake`, …), so the project is intended to be built out-of-tree with CMake/CTest rather than a hand-written Makefile — note `Makefile` itself is gitignored as generated output.
- **Remote:** `git@github.com:sinhaparth5/gittop.git`, default branch `master`.

## When adding the first build

Once `CMakeLists.txt` exists, the expected workflow is the CMake standard:

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Debug -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cmake --build build -j
ctest --test-dir build --output-on-failure
ctest --test-dir build -R <test-name>   # single test
```

Update this section with the project's actual targets, dependencies, and test invocation as soon as they are defined.
