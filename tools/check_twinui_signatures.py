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
        src, re.S)
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
    ap.add_argument("--emit-tsv", type=Path, default=None,
                    help="write normalized_name<TAB>0xRVA rows for known-symbol tables")
    args = ap.parse_args(argv)

    src, _, patterns = load_patterns(args.source)
    print(f"patterns={len(patterns)}")
    data = args.dll.read_bytes()
    pe = pefile.PE(str(args.dll), fast_load=True)
    failed = False
    tsv_rows = []
    for label, pat in patterns:
        hits = find_matches(data, pat)
        if len(hits) == 1:
            rva, sec = offset_to_rva(pe, hits[0])
            print(f"OK   {label}: RVA=0x{rva:X} {sec}")
            tsv_rows.append((label, rva))
        else:
            failed = True
            print(f"FAIL {label}: {len(hits)} matches")

    if args.emit_tsv:
        args.emit_tsv.write_text(
            "".join(f"{n}\t0x{rva:X}\t0x0\n" for n, rva in tsv_rows),
            encoding="utf-8")
        print(f"wrote {args.emit_tsv} ({len(tsv_rows)} rows)")

    print("\nnon-log-only hooks:")
    pattern_labels = {label for label, _ in patterns}
    log_only_prefixes = ("Hook_TT_", "Hook_TD_", "Hook_StartMenuHost_")
    log_only_names = {
        "Hook_TP_LoadLogoIfCacheNotUsed",
        "Hook_TP_PublishAllStartViewCached",
        "Hook_TP_PublishLogoAndAppSpace",
        "Hook_TP_PublishDefaultCached",
        "Hook_TP_StartCachePreLoadResult",
        "Hook_TP_DetermineAvailableMain",
        "Hook_TP_Dispatcher_Create",
        "Hook_TP_Dispatcher_PostEvent",
        "Hook_TP_Dispatcher_WndProc",
    }
    non_log = 0
    print("\nrequired symbols:")
    for name, orig in required_symbols(src):
        covered = any(p in name for p in pattern_labels) or \
            "GetScaleFactorForPart" in name or "g_launcherSettings" in name
        print(f"{'OK  ' if covered else 'MISS'} {name} -> {orig}")
        if not covered:
            failed = True

    # Every table entry, not only the required ones. Log-only hooks are
    # generated by the trace/log macros or the CStartMenuHost wrappers.
    block = src.split("DS_SYMBOL_HOOK rgTWinUIHooks[]", 1)[1]
    block = block.split("\n    };", 1)[0]
    entry_re = re.compile(
        r"\{\s*((?:\s*L\"[^\"]*\"\s*,?)+)\s*\},\s*&([A-Za-z0-9_]+),"
        r"\s*([A-Za-z0-9_]+|nullptr),\s*(true|false)", re.S)
    for m in entry_re.finditer(block):
        names = re.findall(r'L"([^"]*)"', m.group(1))
        hook = m.group(3)
        if hook == "nullptr":
            is_log_only = False
        else:
            is_log_only = hook.startswith(log_only_prefixes) or hook in log_only_names
        if is_log_only:
            continue
        non_log += 1
        short = names[0].split("(", 1)[0].split()[-1].rstrip("*&") if names else ""
        covered = any(p in names[0] or short in p for p in pattern_labels)
        covered = covered or "GetScaleFactorForPart" in (names[0] if names else "")
        covered = covered or "g_launcherSettings" in (names[0] if names else "")
        print(f"{'OK  ' if covered else 'MISS'} {hook} | {names[0] if names else ''}")
        if not covered:
            failed = True
    print(f"non-log-only hooks={non_log}")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
