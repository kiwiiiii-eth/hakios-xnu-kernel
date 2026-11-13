#!/usr/bin/env python3
"""tools/args-equiv.py: what the kernel reads from its boot arguments is the same before and after CONFIG_ARGS_BUILTIN.

  python3 tools/args-equiv.py [--out table.md] [--kernel hakios-kernel]

Before: run 69's whole boot line (tools/args-builtin/r69.cmdline, 597 characters), read by the real pexpert/gen/bootargs.c with no
built-in words. After: the loader's part of that line, then what the device repository's run-darwin-live.sh makes in its
DRYRUN_CMDLINE mode for the kernel file given (the .config's retained arguments only), read by the same parser compiled with the header
that `hakios-config.py kernel` makes. For every name on r69's line, every argument of the configuration (args --full) and every key of
soc.args, both sides are asked as the kernel asks (number, text, present) and must answer the same. The table is made here, not written by
hand. Exit 1 when anything differs, or the line is longer than 240 characters."""
import argparse
import os
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import args_host as H

LIMIT = 240


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", help="write the table (markdown) here too")
    ap.add_argument("--kernel", default=os.path.join(H.ROOT, "hakios-kernel"))
    ap.add_argument("--script", default="run-darwin-live.sh")
    a = ap.parse_args()

    before = H.r69()
    rc, out, err, script_line, phone = H.dryrun(a.script, a.kernel)
    if rc or script_line is None or phone:
        print("args-equiv: the dry run of %s failed (exit %s, phone tool called: %s)\n%s%s" % (a.script, rc, phone, out, err))
        return 1
    after = H.loader_prefix(before) + H.normalize_epoch(script_line)
    full = H.config_py("args", "--full").stdout.split()
    soc = [w for w in H.config_py("builtin").stdout.split() if w.split("=")[0] not in set(H.names_of(" ".join(full)))]
    words = H.builtin_words()

    work = H.tmpdir("args-equiv-")
    full_names, soc_names = set(H.names_of(" ".join(full))), set(H.names_of(" ".join(soc)))

    rows_cmp = H.compare(work, before, after, H.config_py("builtin", "--header").stdout, full_names | soc_names | set(H.names_of(words)))
    origin = set(H.names_of(before))
    rows, bad = [], []
    for n, vb, va, where, same in rows_cmp:
        if not same:
            bad.append(n)
        part = "r69" if n in origin else "-"
        if n in full_names:
            part += ", fajita.config"
        if n in soc_names:
            part += ", soc.args"
        rows.append((n, part.replace("-, ", ""), vb, where, va, "same" if same else "DIFFERENT"))
    names = [r[0] for r in rows]
    lines = ["| argument | where it comes from | before (r69) | after: read from | after | |", "|---|---|---|---|---|---|"]
    lines += ["| %s |" % " | ".join(r) for r in rows]
    table = "\n".join(lines)
    print(table)
    missing = [w for w in H.names_of(" ".join(full) + " " + " ".join(soc)) if w not in set(H.names_of(before))]
    print()
    print("names compared: %d; different: %d%s" % (len(names), len(bad), " (%s)" % ", ".join(bad) if bad else ""))
    if missing:
        print("not on r69's line, so compared with 'not set': %s" % ", ".join(missing))
    print("after-line, as the loader will show it (epoch as r69's): %d characters (limit %d)" % (len(after), LIMIT))
    print(after)
    if a.out:
        with open(a.out, "w", encoding="utf-8") as f:
            f.write(table + "\n\n")
            f.write("names compared: %d; different: %d\n\nafter-line (%d characters):\n\n    %s\n" % (len(names), len(bad), len(after), after))
    return 1 if bad or len(after) > LIMIT else 0


if __name__ == "__main__":
    sys.exit(main())
