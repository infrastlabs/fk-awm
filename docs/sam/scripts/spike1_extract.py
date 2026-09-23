#!/usr/bin/env python3
"""spike1_extract.py — 从 ld 日志抽取未定义符号，并对照静态库判定可解析性。

用法:
  python3 spike1_extract.py <ld_log> <out_undef> [--analyze]

产出:
  <out_undef>       去重排序的未定义符号清单
  stdout            统计 + (加 --analyze 时) 每个符号由哪个 .a 提供
"""
import re
import subprocess
import sys
from collections import defaultdict

TOK = re.compile(r"undefined reference to `([A-Za-z_][A-Za-z0-9_]*)['`]")


def extract(path):
    syms = set()
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            if "undefined reference" not in line:
                continue
            m = TOK.search(line)
            if m:
                syms.add(m.group(1))
    return sorted(syms)


def available_archives():
    out = subprocess.run(
        ["find", "/usr/lib", "/usr/local/lib", "-name", "*.a"],
        capture_output=True, text=True,
    ).stdout
    return sorted(p for p in out.splitlines() if p.strip())


def defined_symbols(archives):
    idx = defaultdict(list)
    total = 0
    for p in archives:
        r = subprocess.run(["nm", "-g", "--defined-only", p],
                           capture_output=True, text=True)
        for line in r.stdout.splitlines():
            parts = line.split()
            if len(parts) >= 3:
                idx[parts[2]].append(p)
                total += 1
    return idx, total


def main():
    log = sys.argv[1]
    out = sys.argv[2]
    analyze = "--analyze" in sys.argv

    syms = extract(log)
    with open(out, "w") as f:
        f.write("\n".join(syms))
        if syms:
            f.write("\n")

    print(f"unique undefined symbols: {len(syms)}")
    if not analyze:
        return

    archs = available_archives()
    idx, total = defined_symbols(archs)
    print(f"static archives: {len(archs)}  indexed symbol entries: {total}")

    prov = defaultdict(list)
    missing = []
    for s in syms:
        opts = idx.get(s)
        if opts:
            prov[opts[0]].append(s)
        else:
            missing.append(s)

    print("\n=== RESOLVED (providing archive -> # symbols) ===")
    for lib, ss in sorted(prov.items(), key=lambda kv: -len(kv[1])):
        print(f"  {len(ss):4d}  {lib}")
    print(f"  --- resolved: {sum(len(v) for v in prov.values())} / {len(syms)}")
    print(f"\n=== UNRESOLVED (potential hard blockers): {len(missing)} ===")
    for s in missing:
        print(f"   {s}")


if __name__ == "__main__":
    main()
