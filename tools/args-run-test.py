#!/usr/bin/env python3
"""tools/args-run-test.py: the run scripts of the device repository (scripts/run-fajita.sh, run-darwin.sh, run-darwin-live.sh)
leave arguments out of the boot line only for a kernel that has them built in.

  python3 tools/args-run-test.py [--kernel hakios-kernel]      (devices/fajita is the device repository)

Only the scripts' DRYRUN_CMDLINE mode is used (the command line is printed and the script stops before fastboot, USB or the phone;
a fastboot, adb and edl that fail and leave a note are first in PATH, and a call to one fails the test). Four cases, each through the
three scripts, with `set -e` on as the scripts have it, and the real parser (tools/args_host.py) reading each boot line with the
built-in words of its kernel:
  1. a kernel with the marker and the configuration's hash (the real hakios-kernel): the retained arguments only, no soc.args words
  2. a kernel without the marker, though the .config says CONFIG_ARGS_BUILTIN=y: the whole line, as run 69 had it
  3. a kernel with a marker for other words (X=1 in them, X now out of the .config; or the .config now without CONFIG_ARGS_BUILTIN):
     the script refuses, exit status not 0, and says to make the kernel again
  4. a kernel repository whose hakios-config.py has no `builtin` and no `--full` (the commit before): marker -> refused,
     no marker -> the whole line"""
import argparse
import hashlib
import os
import re
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import args_host as H

SCRIPTS = ("run-fajita.sh", "run-darwin.sh", "run-darwin-live.sh")
failures = []
BASE = "c6b780b"        # the commit before CONFIG_ARGS_BUILTIN: its hakios-config.py has neither `builtin` nor `args --full`


def check(label, ok, detail=""):
    print("%s %s" % ("ok  " if ok else "FAIL", label))
    if not ok:
        failures.append(label)
        if detail:
            print("     " + str(detail).strip().replace("\n", "\n     ")[:700])


def marker_kernel(work, name, words):
    """A file that holds a marker for `words` the way a kernel does (a NUL-ended string among others) and nothing else."""
    p = os.path.join(work, name)
    with open(p, "wb") as f:
        f.write(b"\0\0some text\0hakios_builtin_args_sha=%s\0more text\0" % hashlib.sha256(words.encode()).hexdigest()[:16].encode())
    return p


def plain_kernel(work, name):
    """A kernel without the marker: the old one of build/ or prebuilt/ if there is one, else a file of no marker."""
    for rel in ("build/hakios-kernel-vt19", "prebuilt/hakios-kernel"):
        p = os.path.join(H.ROOT, rel)
        if os.path.isfile(p) and b"hakios_builtin_args_sha=" not in open(p, "rb").read():
            return p
    p = os.path.join(work, name)
    open(p, "wb").write(b"\0\0no marker here\0")
    return p


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--kernel", default=os.path.join(H.ROOT, "hakios-kernel"))
    a = ap.parse_args()
    work = H.tmpdir("args-run-test-")
    r69 = H.r69()
    prefix = H.loader_prefix(r69)
    r69_script = r69[len(prefix):]                                    # what run-darwin-live.sh made in run 69
    words = H.builtin_words()
    full = H.config_py("args", "--full").stdout.split()
    soc = [l for l in open(os.path.join(H.ROOT, "devices", "fajita", "soc.args")).read().split("\n") if not re.match(r"\s*#", l)]
    socargs = " ".join(" ".join(soc).split())
    sha_now = H.config_py("builtin", "--sha").stdout.strip()

    def before_script(s):
        """What the script made before this change (CONFIG_ARGS_BUILTIN), worked out from its own text: the whole argument line, then
        soc.args words (each word, one space between, a space after the last), then the extra ` ipa8=10`. Run 69's is run-darwin-live.sh's."""
        mid = " ".join(full) + " " + socargs + " " + " ipa8=10"
        if s == "run-darwin-live.sh":
            return r69_script
        if s == "run-darwin.sh":
            return "loader_return_after=300 epoch=1794567272 sb_delay=8  " + mid        # live has PRE as well: one space more
        return "loader_return_after=300 -v " + mid

    # ---- 1: the real kernel ---------------------------------------------------------------------------------------------
    print("== 1. a kernel with the marker and the configuration's hash: %s" % a.kernel)
    line = subprocess.run("strings -a %s | grep -m1 '^hakios_builtin_args_sha='" % a.kernel, shell=True, capture_output=True, text=True).stdout.strip()
    check("the kernel carries the marker, and it is `builtin --sha` (%s)" % sha_now, line == "hakios_builtin_args_sha=" + sha_now, line)
    check("the kernel holds the built-in words (strings -a)", words in subprocess.run(["strings", "-a", a.kernel], capture_output=True, text=True).stdout)
    header_in_tree = os.path.join(H.ROOT, "xnu-src", "xnu-8796.121.2", "pexpert", "gen", "hakios.builtin_args.h")
    if os.path.isfile(header_in_tree):
        text = open(header_in_tree).read()
        check("the generated header in the kernel tree holds soc_cpufreq*, soc_smem, thermal_soc, buttons_*, word for word",
              all(w in text for w in ("soc_cpufreq=0x17d43000 soc_cpufreq_step=0x2800 soc_cpufreq_groups=2 soc_cpufreq_osm=1", "soc_smem=0x86000000",
                                      "thermal_soc=845", "buttons_pon=0x8 buttons_volup=0xc5 buttons_voldown=0xc4")), text)
        check("the generated header is the one `builtin --header` makes now", text == H.config_py("builtin", "--header").stdout)
    retained = H.config_py("args").stdout.strip()
    for s in SCRIPTS:
        rc, out, err, cmd, phone = H.dryrun(s, a.kernel)
        check("%s: exit 0, no phone tool, the retained arguments only" % s, rc == 0 and not phone and cmd is not None and retained in cmd and
              (retained + "  ipa8=10") in cmd, (rc, out, err))
        check("%s: no soc.args word and no built-in argument on the line" % s,
              cmd is not None and not any(n in H.names_of(cmd) for n in H.names_of(words)), cmd)
        if cmd is not None:
            check("%s: the parser reads from it, with the kernel's words, what it read from the whole line the script made before" % s,
                  all(c[4] for c in H.compare(work, prefix + before_script(s), prefix + H.normalize_epoch(cmd), H.config_py("builtin", "--header").stdout,
                                              H.names_of(words), tag="1" + s)))
            check("%s: the whole line is %d characters (the kernel holds 607)" % (s, len(prefix + cmd)), len(prefix + cmd) <= 240)

    # ---- 2: no marker, ARGS_BUILTIN=y -----------------------------------------------------------------------------------
    plain = plain_kernel(work, "plain.kernel")
    print("== 2. a kernel without the marker (%s), CONFIG_ARGS_BUILTIN=y in .config" % (os.path.relpath(plain, H.ROOT) if plain.startswith(H.ROOT) else plain))
    check("the .config says CONFIG_ARGS_BUILTIN=y", "CONFIG_ARGS_BUILTIN=y" in open(os.path.join(H.ROOT, ".config")).read())
    for s in SCRIPTS:
        rc, out, err, cmd, phone = H.dryrun(s, plain)
        got = H.normalize_epoch(cmd or "")
        check("%s: the whole line as before (%s), set -e on" % (s, "byte for byte as run 69's" if s == "run-darwin-live.sh" else "worked out from the old script's text"),
              rc == 0 and not phone and got == before_script(s), (rc, got, before_script(s), err))
        if cmd is not None:
            check("%s: the parser reads the same values as before; the line is %d characters (607 at most)" % (s, len(prefix + cmd)),
                  len(prefix + cmd) <= 607 and all(c[4] for c in H.compare(work, prefix + before_script(s), prefix + H.normalize_epoch(cmd), H.header(None), tag="2" + s)))

    # ---- 3: a marker for other words ------------------------------------------------------------------------------------
    print("== 3. a kernel with a marker for other words")
    stale_words = words + " x_removed=1"
    stale = marker_kernel(work, "stale.kernel", stale_words)
    for s in SCRIPTS:
        rc, out, err, cmd, phone = H.dryrun(s, stale)
        check("%s: refused (exit %s), says to make the kernel again, never reaches the phone, no command line" % (s, rc),
              rc != 0 and "make kernel" in err and cmd is None and not phone, (rc, out, err))
    rc, out, err, cmd, phone = H.dryrun("run-darwin-live.sh", marker_kernel(work, "junk.kernel", "x"), env={})
    check("a marker with a hash that is not the configuration's is refused too", rc != 0 and cmd is None)
    junk = os.path.join(work, "bad.kernel")
    open(junk, "wb").write(b"\0hakios_builtin_args_sha=nothex\0")
    rc, out, err, cmd, phone = H.dryrun("run-darwin.sh", junk)
    check("a marker that is not 16 hex digits is refused", rc != 0 and cmd is None and not phone, (rc, err))
    # why: with the retained line alone the removed argument would still come on in that kernel
    retained_line = prefix + H.normalize_epoch(H.dryrun("run-darwin-live.sh", a.kernel)[3])
    c = H.compare(work, retained_line, retained_line, H.header(stale_words), ["x_removed"], tag="3")
    check("(why) that kernel would read x_removed=1 from its own words though the configuration dropped it", [r for r in c if r[0] == "x_removed"][0][2] == "1", c)
    # the .config without CONFIG_ARGS_BUILTIN, the kernel still with the marker (the way back is a new kernel)
    off = H.make_k(work, "k-off", builtin=False)
    for s in SCRIPTS:
        rc, out, err, cmd, phone = H.dryrun(s, a.kernel, k=off)
        check("%s: .config without CONFIG_ARGS_BUILTIN, kernel with the marker: refused" % s, rc != 0 and "make kernel" in err and cmd is None and not phone, (rc, err))
    for s in SCRIPTS:
        rc, out, err, cmd, phone = H.dryrun(s, plain, k=off)
        check("%s: .config without CONFIG_ARGS_BUILTIN, kernel without the marker: the whole line" % s,
              rc == 0 and cmd is not None and all(w in cmd.split() for w in full) and "soc_smem=0x86000000" in cmd, (rc, err))

    # ---- 4: a kernel repository from before `builtin` ------------------------------------------------------------------
    print("== 4. a kernel repository whose hakios-config.py has no `builtin` and no `args --full` (%s)" % BASE)
    old = H.make_k(work, "k-old", old_tool_ref=BASE, builtin=False)
    probe = subprocess.run([sys.executable, "-I", os.path.join(old, "tools", "hakios-config.py"), "builtin", "--sha"], capture_output=True, text=True, cwd=old)
    check("that tool has no `builtin` (exit %d)" % probe.returncode, probe.returncode != 0)
    old_full = subprocess.run([sys.executable, "-I", os.path.join(old, "tools", "hakios-config.py"), "args"], capture_output=True, text=True, cwd=old).stdout.strip()
    for s in SCRIPTS:
        rc, out, err, cmd, phone = H.dryrun(s, a.kernel, k=old)
        check("%s: kernel with the marker, no way to compute the hash: refused, not an abort in a stray place" % s,
              rc != 0 and "builtin --sha" in err and cmd is None and not phone, (rc, out, err))
        rc, out, err, cmd, phone = H.dryrun(s, plain, k=old)
        check("%s: kernel without the marker: that repository's own `args` and soc.args, the whole line" % s,
              rc == 0 and not phone and cmd is not None and old_full in cmd and "soc_smem=0x86000000" in cmd, (rc, out, err))

    print()
    print("%d failed" % len(failures) if failures else "all passed")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
