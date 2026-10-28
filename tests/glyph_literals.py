#!/usr/bin/env python3
"""Fails on a non-ASCII string literal under src/ui/ outside glyphs.cpp.

A character chosen at a call site is a bet about what the terminal can draw,
and it bypasses the ascii set entirely. The em dash is allowed: it is prose
punctuation used across every toast in app.cpp, not a glyph role.
"""
import pathlib
import re
import sys

ALLOWED = {"—"}
LITERAL = re.compile(r'"(?:[^"\\\n]|\\.)*"')

ui = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else "src/ui")
found = 0
for path in sorted(ui.glob("*.[ch]pp")):
    if path.name == "glyphs.cpp":
        continue
    for number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        code = line.split("//", 1)[0]
        for match in LITERAL.finditer(code):
            bad = {c for c in match.group() if ord(c) > 127} - ALLOWED
            if bad:
                print(f"{path}:{number}: {match.group()} — use a glyph role")
                found += 1
sys.exit(1 if found else 0)
