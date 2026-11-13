#!/usr/bin/env python3
"""tools/args-unchanged-test.py: `hakios-config.py args` prints what it always printed, for every configuration that does not set
CONFIG_ARGS_BUILTIN (QEMU's, the open one, the redwood phone's), and `args --full` prints what the 6T line was before.

  python3 tools/args-unchanged-test.py [--base REF]      (REF: the commit before CONFIG_ARGS_BUILTIN; default c6b780b)

The tool of REF is run beside this repository's configs (a temporary tree of links), and the tool now is run on the same. Each
defconfig is asked alone and with each fragment. The redwood phone's repository is found in devices/redwood, or ../hakios-sm7325;
without one it is left out, and the output says so."""
import argparse
import os
import subprocess
import sys
import atexit
import shutil
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
failures = []


def check(label, ok, detail=""):
    print("%s %s" % ("ok  " if ok else "FAIL", label))
    if not ok:
        failures.append(label)
        if detail:
            print("     " + str(detail)[:600])


def run(tool, root, *args):
    r = subprocess.run([sys.executable, "-I", tool, "--root", root] + list(args), capture_output=True, text=True)
    return r.returncode, r.stdout, r.stderr


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--base", default="c6b780b")
    a = ap.parse_args()
    t = tempfile.mkdtemp(prefix="args-unchanged-")
    atexit.register(shutil.rmtree, t, True)
    os.makedirs(os.path.join(t, "tools"))
    os.makedirs(os.path.join(t, "devices"))
    base_tool = os.path.join(t, "tools", "hakios-config.py")
    src = subprocess.run(["git", "-C", ROOT, "show", "%s:tools/hakios-config.py" % a.base], capture_output=True, text=True, check=True).stdout
    open(base_tool, "w").write(src)
    for name in ("configs", "drivers", "socs", "VERSION", "docs"):
        if os.path.exists(os.path.join(ROOT, name)):
            os.symlink(os.path.join(ROOT, name), os.path.join(t, name))
    for name in os.listdir(os.path.join(ROOT, "devices")):
        if name != "fajita":
            os.symlink(os.path.realpath(os.path.join(ROOT, "devices", name)), os.path.join(t, "devices", name))
    redwood = [p for p in (os.path.join(ROOT, "devices", "redwood"), os.path.join(os.path.dirname(ROOT), "hakios-sm7325"),
                           os.path.join(os.path.dirname(ROOT), "hakios-xnu-kernel", "..", "hakios-sm7325")) if os.path.isdir(p)]
    if redwood and not os.path.exists(os.path.join(t, "devices", "redwood")):
        os.symlink(os.path.realpath(redwood[0]), os.path.join(t, "devices", "redwood"))
    names = ["defconfig", "common", "qemu"] + (["redwood"] if os.path.isdir(os.path.join(t, "devices", "redwood")) else [])
    if "redwood" not in names:
        print("note: no redwood repository found (devices/redwood or ../hakios-sm7325): it is not compared")
    new_tool = os.path.join(HERE, "hakios-config.py")
    for d in names:
        for frag in ([], ["open"], ["trace"], ["virt"], ["persist"], ["open", "trace"]):
            args = ["args", "--defconfig", d, "--force"] + sum((["--fragment", f] for f in frag), [])
            old, new = run(base_tool, t, *args), run(new_tool, t, *args)
            check("%s + %s: `args` as before (%d characters)" % (d, "+".join(frag) or "nothing", len(old[1])), old == new, (old, new))
    # the 6T (devices/fajita is this tree's own): with CONFIG_ARGS_BUILTIN the full line is the one the base tool printed
    config = os.path.join(t, "fajita.config")
    r = subprocess.run([sys.executable, "-I", new_tool, "--root", ROOT, "defconfig", "fajita", "-o", config], capture_output=True, text=True)
    full = run(new_tool, ROOT, "--config", config, "args", "--full")
    plain = run(new_tool, ROOT, "--config", config, "args")
    stripped = os.path.join(t, "stripped.config")
    open(stripped, "w").write("".join(l for l in open(config) if "ARGS_BUILTIN" not in l))
    off = run(new_tool, ROOT, "--config", stripped, "args")
    check("the 6T: `args --full` is the whole line, as it was before CONFIG_ARGS_BUILTIN (%d characters)" % len(full[1]), r.returncode == 0 and full[0] == 0 and full[1] == off[1], (full, off))
    check("the 6T: `args` holds only what the loader and user space read (%s)" % plain[1].strip(), plain[1].strip() == "cpus=8 bootcpu=7 ufs=2 gpu=1 wifi=1 haptic=5 ca_vbl=1 torch=3", plain)
    print()
    print("%d failed" % len(failures) if failures else "all passed")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
