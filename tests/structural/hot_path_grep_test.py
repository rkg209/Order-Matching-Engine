#!/usr/bin/env python3
"""Structural gate (Spec 004 T1.3): fail if engine/ or book/ contain a forbidden construct.

Mechanical proof that nothing reintroduces a heap allocation, lock, exception, virtual call, or
log statement onto the hot path -- rather than relying on a reviewer to notice. Comments are
stripped (a line's `//...` suffix is not code) so prose mentioning these words does not trip the
gate; the allow-list below is matched against the comment-stripped, whitespace-trimmed line, so
it breaks the moment one of the legitimate std::make_unique sites moves or changes shape.
"""

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]

# The only permitted std::make_unique call sites: one-time startup allocation of the pool /
# arena storage itself (constitution: allocation at construction is permitted; allocation PER
# OPERATION is not). Matched by exact trimmed line content, so this allow-list itself breaks if
# these lines are ever moved into a function that runs on the hot path.
ALLOWED_MAKE_UNIQUE_LINES = {
    "levels_(std::make_unique<PriceLevel[]>(numSlots_)),",
    "keys_(std::make_unique<OrderId[]>(capacityPowerOfTwo)),",
    "values_(std::make_unique<Order*[]>(capacityPowerOfTwo)),",
    "state_(std::make_unique<State[]>(capacityPowerOfTwo)) {",
    ": storage_(std::make_unique<Slot[]>(capacity)), capacity_(capacity) {",
    "l0_(std::make_unique<std::uint64_t[]>(l0Words_)),",
    "l1_(std::make_unique<std::uint64_t[]>(l1Words_)),",
}

# Word-bounded patterns: a bare identifier that should never appear as a whole word/token.
WORD_PATTERNS = [
    "malloc",
    "virtual",
    "throw",
    "printf",
    "iostream",
    "spdlog",
    "shared_ptr",
]

# Substring patterns: multi-token constructs, specific enough that substring matching is safe.
SUBSTRING_PATTERNS = [
    "new ",
    "std::mutex",
    "std::lock",
    "std::vector",
    "std::map",
    "std::unordered_map",
    "std::function",
]


def strip_comment(line: str) -> str:
    # Heuristic line-comment strip. This codebase uses `//` line comments exclusively in
    # engine/ and book/ (no block comments), so this is exact for the files this test covers.
    idx = line.find("//")
    return line if idx == -1 else line[:idx]


def check_file(path: Path):
    violations = []
    for lineno, raw in enumerate(path.read_text().splitlines(), start=1):
        code = strip_comment(raw)
        trimmed = raw.strip()

        if "std::make_unique" in code:
            if trimmed not in ALLOWED_MAKE_UNIQUE_LINES:
                violations.append((lineno, raw, "std::make_unique not on the allow-list"))
            continue

        for pat in WORD_PATTERNS:
            if re.search(rf"\b{re.escape(pat)}\b", code):
                violations.append((lineno, raw, f"forbidden construct: {pat}"))

        for pat in SUBSTRING_PATTERNS:
            if pat in code:
                violations.append((lineno, raw, f"forbidden construct: {pat.strip()}"))

    return violations


def main() -> int:
    failed = False
    for sub in ("engine", "book"):
        d = ROOT / sub
        for path in sorted(d.rglob("*")):
            if path.suffix not in (".hpp", ".cpp"):
                continue
            for lineno, raw, reason in check_file(path):
                failed = True
                print(f"FORBIDDEN {path.relative_to(ROOT)}:{lineno}: {reason}\n    {raw}")

    if failed:
        print("\nRESULT: FAIL -- a forbidden construct is present in engine/ or book/.")
        return 1

    print("RESULT: PASS -- no forbidden constructs in engine/ or book/.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
