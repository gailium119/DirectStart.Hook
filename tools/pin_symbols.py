#!/usr/bin/env python3
"""Pin known module functions by SHA-256.

This does not guess addresses. Addresses are recorded once (from PDB, IDA or a
log) and then applied to DirectStart.ini only when the module SHA-256 matches.

Examples:
  # Print the hash and add addresses for this exact binary
  python tools/pin_symbols.py --module C:/Windows/twinui.dll \
      --database tools/known_symbols.json \
      --symbol "CImmersiveColor::GetColor=0x1234" \
      --symbol "CPearlResourceManager::_UpdateLayerVisibility=0x5678"

  # Apply all addresses stored for the current hash to the INI
  python tools/pin_symbols.py --module C:/Windows/twinui.dll \
      --database tools/known_symbols.json \
      --ini %LOCALAPPDATA%/DirectStart/DirectStart.ini --apply

The database layout is:

{
  "twinui.dll": {
    "<sha256>": {
      "CImmersiveColor::GetColor": "module+0x1234"
    }
  }
}
"""

import argparse
import configparser
import hashlib
import json
import os
import sys
from pathlib import Path


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest().upper()


def module_key(path: Path) -> str:
    return path.name.lower()


def section_default(path: Path) -> str:
    return path.stem.lower()


def normalize_address(value: str) -> str:
    value = value.strip()
    low = value.lower()
    if low.startswith(("module+", "rva+", "va+")):
        return value
    if low.startswith("0x"):
        return "module+" + value
    # bare hex is treated as a module RVA
    return "module+0x" + value


def load_db(path: Path) -> dict:
    if not path.exists():
        return {}
    return json.loads(path.read_text(encoding="utf-8"))


def save_db(path: Path, db: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(
        json.dumps(db, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )


def read_ini(path: Path) -> configparser.ConfigParser:
    parser = configparser.ConfigParser(interpolation=None)
    parser.optionxform = str
    if path.exists():
        for encoding in ("utf-8-sig", "utf-8", "mbcs"):
            try:
                parser.read(path, encoding=encoding)
                break
            except (UnicodeDecodeError, LookupError):
                parser = configparser.ConfigParser(interpolation=None)
                parser.optionxform = str
    return parser


def write_ini(path: Path, parser: configparser.ConfigParser) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8", newline="\r\n") as f:
        parser.write(f)


def normalize_symbol_name(value: str) -> str:
    # Mirrors DirectStart's NormalizeSymbolName: drop the signature, keep the
    # trailing token, and strip trailing pointer/reference markers.
    text = value.split("(", 1)[0].rstrip()
    token = text.split()[-1] if text.split() else ""
    return token.rstrip("*&")


def parse_symbol_args(values):
    out = {}
    for item in values or []:
        if "=" not in item:
            raise SystemExit(f"bad --symbol (expected NAME=RVA): {item}")
        name, address = item.split("=", 1)
        name = normalize_symbol_name(name)
        if not name:
            raise SystemExit(f"bad --symbol name: {item}")
        out[name] = normalize_address(address)
    return out


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--module", required=True, type=Path)
    ap.add_argument("--section", default=None, help="INI section (default: module stem)")
    ap.add_argument("--ini", type=Path, default=None, help="DirectStart.ini path")
    ap.add_argument("--database", type=Path, default=Path("tools/known_symbols.json"))
    ap.add_argument("--symbol", action="append", metavar="NAME=RVA",
                    help="record/apply one function address; repeatable")
    ap.add_argument("--apply", action="store_true",
                    help="write addresses for this SHA-256 into the INI")
    ap.add_argument("--print-hash", action="store_true")
    ap.add_argument("--list", action="store_true",
                    help="list known hashes for this module")
    args = ap.parse_args(argv)

    module = args.module.expanduser()
    if not module.is_file():
        raise SystemExit(f"module not found: {module}")

    digest = sha256_file(module)
    key = module_key(module)
    section = args.section or section_default(module)
    db = load_db(args.database)
    module_db = db.setdefault(key, {})

    if args.print_hash:
        print(f"{key} {digest}")

    if args.list:
        for sha in sorted(module_db):
            print(f"{sha} {len(module_db[sha])} symbols")
        return 0

    symbols = parse_symbol_args(args.symbol)
    if symbols:
        module_db.setdefault(digest, {}).update(symbols)
        save_db(args.database, db)
        print(f"recorded {len(symbols)} symbols for {key} {digest}")

    if args.apply:
        if args.ini is None:
            raise SystemExit("--apply requires --ini")
        if not symbols:
            symbols = module_db.get(digest, {})
            if not symbols:
                raise SystemExit(
                    f"no addresses stored for {key} {digest}; "
                    "add them with --symbol first")
        parser = read_ini(args.ini)
        if not parser.has_section(section):
            parser.add_section(section)
        for name, address in symbols.items():
            parser.set(section, name, address)
        write_ini(args.ini, parser)
        print(f"applied {len(symbols)} symbols to [{section}] in {args.ini}")

    if not symbols and not args.apply and not args.print_hash and not args.list:
        print(f"{key} SHA256={digest}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
