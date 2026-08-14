#!/usr/bin/env python3
"""DLCVCAM boot-try record on misc (must match U-Boot fit.c layout).

misc @ 48KiB:
  magic 'DCBT' (LE 0x54424344), version, try_count, limit, flags, reserved[3]

Userspace clears try_count after a successful boot (multi-user) so the next
cold boot does not accumulate toward the U-Boot recovery threshold.
"""
from __future__ import annotations

import argparse
import os
import struct
import sys
from pathlib import Path

MAGIC = 0x54424344  # 'DCBT'
VERSION = 1
OFF = 48 * 1024
DEFAULT_LIMIT = 2
FLAG_STICKY = 1 << 0
FMT = "<IIIII3I"  # magic, version, try_count, limit, flags, reserved[3]
SIZE = struct.calcsize(FMT)
assert SIZE == 32

MISC = Path(os.environ.get("DLCVCAM_MISC_PART", "/dev/disk/by-partlabel/misc"))


def _need_root() -> None:
    if os.geteuid() != 0:
        sys.exit("must run as root")


def read_record(path: Path = MISC) -> dict:
    with path.open("rb") as f:
        f.seek(OFF)
        raw = f.read(SIZE)
    if len(raw) != SIZE:
        raise RuntimeError("short read on misc")
    magic, ver, try_count, limit, flags, r0, r1, r2 = struct.unpack(FMT, raw)
    return {
        "raw_ok": magic == MAGIC and ver == VERSION,
        "magic": magic,
        "version": ver,
        "try_count": try_count,
        "limit": limit if limit else DEFAULT_LIMIT,
        "flags": flags,
        "sticky": bool(flags & FLAG_STICKY),
        "reserved": (r0, r1, r2),
    }


def write_record(
    *,
    try_count: int,
    limit: int = DEFAULT_LIMIT,
    sticky: bool = False,
    path: Path = MISC,
) -> None:
    flags = FLAG_STICKY if sticky else 0
    blob = struct.pack(
        FMT,
        MAGIC,
        VERSION,
        int(try_count) & 0xFFFFFFFF,
        int(limit) & 0xFFFFFFFF,
        flags,
        0,
        0,
        0,
    )
    with path.open("r+b", buffering=0) as f:
        f.seek(OFF)
        f.write(blob)
        f.flush()
        os.fsync(f.fileno())


def clear_bcb(path: Path = MISC) -> None:
    """Clear Android BCB command at offset 0 and 16KiB (stock dual layout)."""
    zeros = b"\x00" * 2048
    with path.open("r+b", buffering=0) as f:
        for off in (0, 16 * 1024):
            f.seek(off)
            f.write(zeros)
        f.flush()
        os.fsync(f.fileno())


def cmd_status(_: argparse.Namespace) -> int:
    rec = read_record()
    print(f"misc={MISC}")
    print(f"offset={OFF} size={SIZE}")
    print(f"valid={rec['raw_ok']}")
    print(f"try_count={rec['try_count']}")
    print(f"limit={rec['limit']}")
    print(f"flags=0x{rec['flags']:x} sticky={rec['sticky']}")
    return 0


def cmd_confirm(ns: argparse.Namespace) -> int:
    """Successful boot: clear try_count and sticky; optionally clear BCB."""
    rec = read_record()
    limit = rec["limit"] if rec["raw_ok"] and rec["limit"] else DEFAULT_LIMIT
    if ns.limit is not None:
        limit = ns.limit
    write_record(try_count=0, limit=limit, sticky=False)
    if not ns.keep_bcb:
        clear_bcb()
    print("confirm: try_count=0 sticky=0" + ("" if ns.keep_bcb else " bcb_cleared"))
    return 0


def cmd_reset(ns: argparse.Namespace) -> int:
    """Before flashing a new kernel: arm a fresh trial window."""
    limit = ns.limit if ns.limit is not None else DEFAULT_LIMIT
    write_record(try_count=0, limit=limit, sticky=False)
    clear_bcb()
    print(f"reset: try_count=0 limit={limit} sticky=0 bcb_cleared")
    return 0


def cmd_set(ns: argparse.Namespace) -> int:
    write_record(
        try_count=ns.try_count,
        limit=ns.limit if ns.limit is not None else DEFAULT_LIMIT,
        sticky=ns.sticky,
    )
    print("set ok")
    return 0


def main() -> int:
    p = argparse.ArgumentParser(description="DLCVCAM boot-try (misc@48KiB)")
    sub = p.add_subparsers(dest="cmd", required=True)

    s = sub.add_parser("status", help="show boot-try record")
    s.set_defaults(func=cmd_status)

    s = sub.add_parser("confirm", help="successful boot: clear counter/sticky")
    s.add_argument("--keep-bcb", action="store_true")
    s.add_argument("--limit", type=int, default=None)
    s.set_defaults(func=cmd_confirm)

    s = sub.add_parser("reset", help="arm trial before kernel flash")
    s.add_argument("--limit", type=int, default=None)
    s.set_defaults(func=cmd_reset)

    s = sub.add_parser("set", help="debug: set fields")
    s.add_argument("--try-count", type=int, required=True)
    s.add_argument("--limit", type=int, default=None)
    s.add_argument("--sticky", action="store_true")
    s.set_defaults(func=cmd_set)

    ns = p.parse_args()
    _need_root()
    if not MISC.exists():
        sys.exit(f"missing {MISC}")
    return ns.func(ns)


if __name__ == "__main__":
    sys.exit(main())
