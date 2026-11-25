#!/usr/bin/env python3
"""Static real-time safety scan for audio-thread processBlock/processAudio bodies."""

import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SOURCE_DIR = os.path.join(ROOT, "Source")

FORBIDDEN = [
    ("callAsync", re.compile(r"\bcallAsync\b")),
    ("MessageManagerLock", re.compile(r"\bMessageManagerLock\b")),
    ("AlertWindow", re.compile(r"\bAlertWindow\b")),
    ("ScopedLock", re.compile(r"\b(?:juce::)?ScopedLock\b")),
    ("std::lock_guard", re.compile(r"\bstd::lock_guard\b")),
    ("std::unique_lock", re.compile(r"\bstd::unique_lock\b")),
    ("C heap alloc/free", re.compile(r"\b(?:malloc|calloc|realloc|free)\s*\(")),
]


def strip_comments_and_strings(src: str) -> str:
    """Replace comments and string/char literals with spaces, preserving newlines."""
    out = []
    i = 0
    n = len(src)
    while i < n:
        ch = src[i]
        # Raw string literal: R"delim(...)delim"
        if ch == "R" and i + 1 < n and src[i + 1] == '"':
            j = i + 2
            while j < n and src[j] != "(" and src[j] != "\n":
                j += 1
            if j < n and src[j] == "(":
                delim = ")" + src[i + 2 : j] + '"'
                end = src.find(delim, j + 1)
                if end != -1:
                    seg = src[i : end + len(delim)]
                    out.append("".join("\n" if c == "\n" else " " for c in seg))
                    i = end + len(delim)
                    continue
        # Line comment
        if ch == "/" and i + 1 < n and src[i + 1] == "/":
            while i < n and src[i] != "\n":
                out.append(" ")
                i += 1
            continue
        # Block comment
        if ch == "/" and i + 1 < n and src[i + 1] == "*":
            out.append("  ")
            i += 2
            while i < n and not (src[i] == "*" and i + 1 < n and src[i + 1] == "/"):
                out.append("\n" if src[i] == "\n" else " ")
                i += 1
            if i < n:
                out.append("  ")
                i += 2
            continue
        # String or char literal
        if ch in ('"', "'"):
            quote = ch
            out.append(" ")
            i += 1
            while i < n:
                c = src[i]
                if c == "\\":
                    out.append("\n" if c == "\n" else " ")
                    i += 1
                    if i < n:
                        out.append("\n" if src[i] == "\n" else " ")
                        i += 1
                    continue
                if c == quote:
                    out.append(" ")
                    i += 1
                    break
                out.append("\n" if c == "\n" else " ")
                i += 1
            continue
        out.append(ch)
        i += 1
    return "".join(out)


def find_matching_delim(text: str, open_idx: int, open_ch: str, close_ch: str) -> int:
    depth = 0
    i = open_idx
    n = len(text)
    while i < n:
        if text[i] == open_ch:
            depth += 1
        elif text[i] == close_ch:
            depth -= 1
            if depth == 0:
                return i
        i += 1
    return -1


def extract_audio_bodies(stripped: str):
    """Yield (fn_name, start_line, body_text, body_start_line) for definitions."""
    sig_re = re.compile(r"\b(processBlock|processAudio)\s*\(")
    pos = 0
    while True:
        m = sig_re.search(stripped, pos)
        if not m:
            break
        fn_name = m.group(1)
        open_paren = m.end() - 1
        close_paren = find_matching_delim(stripped, open_paren, "(", ")")
        if close_paren == -1:
            pos = m.end()
            continue

        # Check preceding non-whitespace character so member calls (e.g. a.processBlock(...))
        # and returns don't look like definitions, and require '{' after param qualifiers.
        k = close_paren + 1
        while k < len(stripped):
            tail = stripped[k:]
            ws = re.match(r"\s+", tail)
            if ws:
                k += ws.end()
                continue
            qual = re.match(r"(?:const|noexcept|override|final)\b", tail)
            if qual:
                k += qual.end()
                continue
            break

        if k < len(stripped) and stripped[k] == "{":
            # Guard against control flow / calls: check prefix on same signature doesn't have '.' or '->'
            prefix_start = max(0, m.start() - 32)
            prefix = stripped[prefix_start : m.start()].rstrip()
            if not prefix.endswith(".") and not prefix.endswith("->"):
                close_brace = find_matching_delim(stripped, k, "{", "}")
                if close_brace != -1:
                    start_line = stripped[: m.start()].count("\n") + 1
                    body_start_line = stripped[: k + 1].count("\n") + 1
                    yield fn_name, start_line, stripped[k + 1 : close_brace], body_start_line
                    pos = close_brace + 1
                    continue

        pos = m.end()


def main() -> int:
    source_files = []
    for root, _, files in os.walk(SOURCE_DIR):
        for fname in sorted(files):
            if fname.endswith((".h", ".hpp", ".cpp", ".cc")):
                source_files.append(os.path.join(root, fname))
    source_files.sort()

    scanned_fns = 0
    violations = []

    for path in source_files:
        with open(path, "r", encoding="utf-8", errors="replace") as f:
            raw = f.read()
        stripped = strip_comments_and_strings(raw)
        rel_path = os.path.relpath(path, ROOT).replace("\\", "/")

        for fn_name, _fn_line, body, body_start_line in extract_audio_bodies(stripped):
            scanned_fns += 1
            for offset, line in enumerate(body.splitlines()):
                lineno = body_start_line + offset
                for label, rx in FORBIDDEN:
                    if rx.search(line):
                        if label == "std::unique_lock" and "try_to_lock" in line:
                            continue
                        violations.append((rel_path, lineno, fn_name, label, line.strip()))

    print(f"audio_path_scan: scanned {scanned_fns} processBlock/processAudio bodies across {len(source_files)} files")
    for rel_path, lineno, fn_name, label, snippet in violations:
        print(f"  [FAIL] {rel_path}:{lineno} ({fn_name}): forbidden '{label}' -> {snippet}")

    if violations:
        print(f"audio_path_scan: FAILED ({len(violations)} violation(s))")
        return 1

    print("audio_path_scan: OK (0 violations)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
