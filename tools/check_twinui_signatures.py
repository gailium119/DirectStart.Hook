#!/usr/bin/env python3
"""Audit TWinUI signature coverage and verify patterns against a DLL.

- Parses the fallback patterns from StartMenuTwinUI.cpp.
- Verifies every required symbol table entry has a pattern (or is derived from
  the launcher-settings pattern).
- Scans the given TWinUI.dll for each pattern and reports 0 / multiple matches.
"""

import argparse
import re
import sys
from pathlib import Path

import pefile


def c_string_concat(text: str) -> str:
    return "".join(re.findall(r'"([^"]*)"', text))


def load_patterns(source: Path):
    src = source.read_text(encoding="utf-8", errors="replace")
    block = src.split("static BOOL InitializeTWinUIHooksByPattern", 1)[1]
    block = block.split("\n}", 1)[0]

    pats = []
    call_re = re.compile(
        r"(?:InstallTWinUIHookByPattern|ResolveTWinUIFunctionByPattern)"
        r"\(\s*hTwinUI,\s*((?:\"(?:[^\"\\]|\\.)*\"\s*)+),\s*L\"([^\"]+)\"",
        re.S,
    )
    for m in call_re.finditer(block):
        pats.append((m.group(2), c_string_concat(m.group(1))))

    # ResolveLauncherSettingsByPattern has a local const char* pattern and
    # resolves GetScaleFactorForPart.
    m = re.search(
        r"const char\* pattern\s*=\s*((?:\"(?:[^\"\\]|\\.)*\"\s*)+);",
        block, re.S)
    if m:
        pats.append(("CLauncherSettings::GetScaleFactorForPart",
                     c_string_concat(m.group(1))))
    return src, block, pats


def required_symbols(src: str):
    block = src.split("DS_SYMBOL_HOOK rgTWinUIHooks[]", 1)[1]
    block = block.split("\n    };", 1)[0]
    required = []
    entry_re = re.compile(
        r"\{\s*((?:\s*L\"[^\"]*\"\s*,?)+)\s*\},\s*&([A-Za-z0-9_]+),"
        r"\s*([A-Za-z0-9_]+|nullptr),\s*(true|false)",
        re.S,
    )
    for m in entry_re.finditer(block):
        if m.group(4) == "false":
            names = re.findall(r'L"([^"]*)"', m.group(1))
            required.append((names[0] if names else "", m.group(2)))
    return required


def pattern_tokens(pattern: str):
    tokens = []
    for col in pattern.split():
        if col == "?":
            tokens.append(None)
        else:
            tokens.append(int(col, 16))
    return tokens


def find_matches(data: bytes, pattern: str):
    tokens = pattern_tokens(pattern)
    if not tokens or tokens[0] is None:
        return []
    first = bytes([tokens[0]]) if tokens[0] is not None else b""
    hits = []
    pos = 0
    while True:
        i = data.find(first, pos)
        if i < 0:
            break
        ok = True
        for j, tok in enumerate(tokens):
            if tok is not None and i + j >= len(data):
                ok = False
                break
            if tok is not None and data[i + j] != tok:
                ok = False
                break
        if ok:
            hits.append(i)
        pos = i + 1
    return hits


def offset_to_rva(pe, offset):
    for s in pe.sections:
        start = s.PointerToRawData
        end = start + s.SizeOfRawData
        if start <= offset < end:
            return s.VirtualAddress + (offset - start), s.Name.rstrip(b"\0").decode("latin1")
    return None, "OVERLAY"


def main(argv=None) -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--source", type=Path,
                    default=Path("DirectStart.Hook/StartMenuTwinUI.cpp"))
    ap.add_argument("--dll", type=Path,
                    default=Path("_ref/twinui-symbols.dll"))
    args = ap.parse_args(argv)

    src, _, patterns = load_patterns(args.source)
    print(f"patterns={len(patterns)}")
    data = args.dll.read_bytes()
    pe = pefile.PE(str(args.dll), fast_load=True)
    failed = False
    for label, pat in patterns:
        hits = find_matches(data, pat)
        if len(hits) == 1:
            rva, sec = offset_to_rva(pe, hits[0])
            print(f"OK   {label}: RVA=0x{rva:X} {sec}")
        else:
            failed = True
            print(f"FAIL {label}: {len(hits)} matches")

    print("\nrequired symbols:")
    pattern_labels = {label for label, _ in patterns}
    derived = {"CLauncherSettings::GetScaleFactorForPart",
               "g_launcherSettings"}
    for name, orig in required_symbols(src):
        short = name
        for prefix in ("public: ", "private: ", "protected: "):
            short = short.replace(prefix, "")
        covered = any(p in name or name.split("(")[0].split("::")[-1] in p
                      for p in pattern_labels) or short in derived
        covered = covered or "GetScaleFactorForPart" in name or "g_launcherSettings" in name
        print(f"{'OK  ' if covered else 'MISS'} {name} -> {orig}")
        if not covered:
            failed = True
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
