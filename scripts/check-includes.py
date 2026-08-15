#!/usr/bin/env python3
"""Report standard-library symbols used without the header that declares them.

This exists because of one specific way the Windows build can break that nothing
else in this repository will catch. A standard header is permitted to include
another, and which symbols arrive that way is a quality-of-implementation
matter: `std::back_inserter` comes through <vector> on libstdc++ and does not on
MSVC's STL. A file missing `#include <iterator>` therefore compiles cleanly with
GCC, compiles cleanly under the mingw cross-build in scripts/build-windows.sh —
which is also libstdc++ — and fails only on the MSVC job, which runs on a tag.

That is the worst possible place to find out: the tag already exists, the
release does not, and the fix needs the tag moved. `git/transfer.cpp` did
exactly this on the first tagged Windows release.

Neither compiler is wrong, so the fix is always to add the include rather than
to argue with one of them.

    scripts/check-includes.py          # exits non-zero if anything is missing

The symbol table below is deliberately not exhaustive. It covers the names that
are actually used in this codebase and are plausibly transitive; adding a symbol
nobody writes would only be a chance to get its header wrong.
"""

import collections
import pathlib
import re
import sys

SRC = pathlib.Path(__file__).resolve().parent.parent / "src"

NEED = {
    "<algorithm>": ["all_of", "any_of", "clamp", "copy", "copy_if", "count", "count_if",
                    "equal", "fill", "find", "find_if", "for_each", "lower_bound",
                    "max_element", "min_element", "mismatch", "none_of", "partition",
                    "remove", "remove_if", "replace", "reverse", "rotate", "set_difference",
                    "set_intersection", "sort", "stable_sort", "transform", "unique",
                    "upper_bound"],
    "<array>": ["array"],
    "<atomic>": ["atomic"],
    "<cctype>": ["isalnum", "isalpha", "isdigit", "isspace", "isxdigit", "tolower", "toupper"],
    "<chrono>": ["duration_cast", "milliseconds", "steady_clock", "system_clock"],
    "<cmath>": ["ceil", "exp", "fabs", "floor", "fmod", "log", "log2", "pow", "round", "sqrt"],
    "<cstddef>": ["ptrdiff_t", "size_t"],
    "<cstdint>": ["int8_t", "int32_t", "int64_t", "uint8_t", "uint16_t", "uint32_t", "uint64_t"],
    "<cstdio>": ["fclose", "fflush", "fopen", "fprintf", "fputs", "fread", "fwrite", "snprintf"],
    "<cstdlib>": ["atoi", "exit", "getenv", "strtod", "strtol"],
    "<cstring>": ["memcmp", "memcpy", "memmove", "memset", "strchr", "strcmp", "strerror",
                  "strlen", "strncmp"],
    "<fstream>": ["ifstream", "ofstream"],
    "<functional>": ["bind", "cref", "function", "greater", "hash", "less", "ref"],
    "<initializer_list>": ["initializer_list"],
    "<iomanip>": ["get_time", "put_time", "setfill", "setprecision", "setw"],
    "<iterator>": ["advance", "back_inserter", "distance", "front_inserter", "inserter",
                   "istream_iterator", "next", "ostream_iterator", "prev"],
    "<limits>": ["numeric_limits"],
    "<map>": ["map", "multimap"],
    "<memory>": ["make_shared", "make_unique", "shared_ptr", "unique_ptr", "weak_ptr"],
    "<mutex>": ["lock_guard", "mutex", "scoped_lock", "unique_lock"],
    "<numeric>": ["accumulate", "inner_product", "iota", "partial_sum", "reduce"],
    "<optional>": ["nullopt", "optional"],
    "<set>": ["multiset", "set"],
    "<sstream>": ["istringstream", "ostringstream", "stringstream"],
    "<string>": ["stod", "stoi", "stoll", "to_string"],
    "<string_view>": ["string_view"],
    "<unordered_map>": ["unordered_map"],
    "<utility>": ["exchange", "forward", "move", "pair"],
    "<vector>": ["vector"],
}
SYMBOL_HEADER = {sym: header for header, syms in NEED.items() for sym in syms}


def is_standard(header: str) -> bool:
    """<vector> and <cstddef>, but not <ftxui/dom/elements.hpp> or <git2.h>."""
    inner = header[1:-1]
    return "/" not in inner and not inner.endswith(".h")


def visible_headers(path: pathlib.Path, seen: set[str] | None = None) -> set[str]:
    """Every <> include reachable from `path`, following project "" includes.

    Following the project's own headers is what keeps this quiet enough to be
    worth running: most files get <vector> and <string> from the header they
    implement, and reporting those would bury the handful that matter.
    """
    if seen is None:
        seen = set()
    key = str(path)
    if key in seen:
        return set()
    seen.add(key)

    try:
        text = path.read_text(encoding="utf-8")
    except OSError:
        return set()

    found = set(re.findall(r'#include\s+(<[^>]+>)', text))
    for relative in re.findall(r'#include\s+"([^"]+)"', text):
        candidate = SRC / relative
        if candidate.exists():
            found |= visible_headers(candidate, seen)
    return found


def used_symbols(text: str) -> str:
    """The file with includes, comments and string literals removed.

    All three are stripped because all three produce false positives: a comment
    explaining why std::move is used is not a use of it, and neither is the
    include line naming the header.
    """
    text = re.sub(r'#include[^\n]*', '', text)
    text = re.sub(r'//[^\n]*', '', text)
    text = re.sub(r'/\*.*?\*/', '', text, flags=re.S)
    return re.sub(r'"(?:[^"\\]|\\.)*"', '""', text)


def main() -> int:
    problems: dict[str, set[tuple[str, str]]] = collections.defaultdict(set)

    for path in sorted(SRC.rglob("*")):
        if path.suffix not in (".cpp", ".hpp"):
            continue
        visible = {h for h in visible_headers(path) if is_standard(h)}
        body = used_symbols(path.read_text(encoding="utf-8"))
        for symbol, header in SYMBOL_HEADER.items():
            if header in visible:
                continue
            if re.search(r'\bstd::' + symbol + r'\b', body):
                problems[str(path.relative_to(SRC.parent))].add((f"std::{symbol}", header))

    if not problems:
        print("every std:: symbol has its header in view")
        return 0

    for name in sorted(problems):
        print(name)
        for symbol, header in sorted(problems[name]):
            print(f"    {symbol:<26} needs {header}")
    total = sum(len(v) for v in problems.values())
    print(f"\n{total} missing include(s) across {len(problems)} file(s)", file=sys.stderr)
    print("these build with libstdc++ and fail on MSVC — add the include", file=sys.stderr)
    return 1


if __name__ == "__main__":
    sys.exit(main())
