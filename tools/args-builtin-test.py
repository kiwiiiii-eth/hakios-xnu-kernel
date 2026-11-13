#!/usr/bin/env python3
"""tools/args-builtin-test.py: the kernel's own pexpert/gen/bootargs.c, compiled on the host, with built-in boot-arguments.

  python3 tools/args-builtin-test.py        (needs a C compiler as `cc`, and xnu-src/ from xnu/apply.sh)

PE_parse_boot_argn and PE_parse_boot_arg_str look in the boot line first, and only then in the words compiled into the kernel
(CONFIG_ARGS_BUILTIN). Each case below asks the real parser, through tools/args_host.py, and compares with what the rules say:
a name=value takes the last one that matches, a -flag the first, a name that is a prefix of another is not that other."""
import os
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import args_host as H

failures = []


def check(label, ok, detail=""):
    print("%s %s" % ("ok  " if ok else "FAIL", label))
    if not ok:
        failures.append(label)
        if detail:
            print("     " + str(detail))


def main():
    work = H.tmpdir("args-builtin-test-")
    words = "alpha=1 beta=0x10 gamma=text -quiet dup=1 dup=2 thermal_soc=845 soc_smem=0x86000000 mac=02:6f:6e:65:36:54"
    on = H.build(work, H.header(words), "on")
    off = H.build(work, H.header(None), "off")

    def q(exe, cmdline, name):
        return H.query(exe, cmdline, [name])[name]       # (found_num, num, found_str, str, found_presence)

    # only the built-in words
    for cmd in (None, "", "   "):
        label = "empty boot line (%r): the built-in words still answer" % cmd
        r = q(on, cmd, "alpha")
        check(label, r == (1, 1, 1, "1", 1), r)
    check("built-in number in hex", q(on, None, "beta")[:2] == (1, 16))
    check("built-in soc address as a number", q(on, None, "soc_smem")[:2] == (1, 0x86000000 - (1 << 32)))     # PE_parse_boot_argn stores 4 bytes: the int32 of it
    r = q(on, None, "gamma")
    check("built-in text through PE_parse_boot_arg_str", r[2:4] == (1, "text"), r)
    r = q(on, None, "mac")
    check("built-in text with colons is text", r[2:4] == (1, "02:6f:6e:65:36:54"), r)
    r = q(on, None, "-quiet")
    check("built-in -flag is 1 and is present", r == (1, 1, 1, "1", 1), r)
    check("a name nowhere is not found", q(on, "x=1", "nothing") == (0, 0, 0, "", 0))
    check("a name that is a prefix of a built-in one is not it (thermal / thermal_soc)", q(on, None, "thermal")[0] == 0 and q(on, None, "thermal_soc")[:2] == (1, 845))
    check("a built-in name that is a prefix of a longer one is not it (soc_ / soc_smem)", q(on, None, "soc_")[0] == 0 and q(on, None, "soc_sme")[0] == 0)
    # the boot line over the built-in words
    check("the boot line overrides the built-in number", q(on, "alpha=5", "alpha")[:2] == (1, 5))
    check("the boot line overrides the built-in text", q(on, "gamma=other", "gamma")[2:4] == (1, "other"))
    check("the boot line carries other names, the built-in ones still answer", q(on, "cpus=8 ufs=2", "beta")[:2] == (1, 16))
    check("an argument on the boot line without a built-in one", q(on, "cpus=8", "cpus")[:2] == (1, 8))
    # duplicates and flags
    check("duplicates on the boot line: the last wins", q(on, "dup=1 x=2 dup=3", "dup")[:2] == (1, 3))
    check("duplicates in the built-in words: the last wins", q(on, None, "dup")[:2] == (1, 2))
    check("the boot line's duplicate beats the built-in one", q(on, "dup=7", "dup")[:2] == (1, 7))
    check("-flag on the boot line", q(on, "-v -quiet", "-quiet") == (1, 1, 1, "1", 1))
    check("-flag only on the boot line, nothing built in", q(on, "-v", "-v")[0] == 1)
    check("-flag is found by its name with the dash only", q(on, "-v", "v")[0] == 0)
    # off: the built-in words do not exist
    check("without CONFIG_ARGS_BUILTIN the words are not read", q(off, None, "alpha")[0] == 0)
    check("without CONFIG_ARGS_BUILTIN the boot line works as it did", q(off, "alpha=5", "alpha")[:2] == (1, 5))
    # XNU's own cases of parse_boot_arg_test (pexpert/gen/bootargs.c), through the public functions: nothing changes with or without words
    vec = [("-x -a b=3 y=42", "-a", "1"), ("-x -a b=3 y=42", "b", "3"), ("-x -a b=2 ba=3 y=42", "b", "2"),
           ("-x -a ba=3 b=2 y=42", "b", "2"), ("-x -a b=2 ba=3 y=42", "ba", "3"), ("hello=world -foobar abc debug=0xBAADF00D", "hello", "world"),
           ("hello=world -foobar abc debug=0xBAADF00D", "debug", "0xBAADF00D"), ("hello=world -foobar abc debug=0xBAADF00D", "-foobar", "1"),
           ("hello=world -foobar abc debug=0xBAADF00D", "abc", "1")]
    bad = []
    for cmd, name, want in vec:
        for exe in (on, off):
            r = q(exe, cmd, name)
            if not (r[2] == 1 and r[3] == want):
                bad.append((cmd, name, want, r))
    check("XNU's own string cases give the same answers with and without built-in words", not bad, bad)
    for cmd, name in (("-x -ab -aa y=42", "-a"), ("-x b=96 y=42", "bx"), ("-x ab=96 y=42", "a"), ("hello=world -foobar abc debug=0xBAADF00D", "notarealthing")):
        check("XNU's not-found case %r %r" % (cmd, name), q(on, cmd, name)[2] == 0 and q(off, cmd, name)[2] == 0)
    # the comparison itself notices: the retained line alone, read by a parser without the words, differs from the whole line
    c = H.compare(work, "cpus=8 cpufreq=1 soc_smem=0x86000000", "cpus=8", H.header(None), tag="neg")
    check("(the comparison can tell) a word missing from the boot line and not built in is a difference", [r[4] for r in c] == [True, False, False], c)
    c = H.compare(work, "cpus=8 cpufreq=1 soc_smem=0x86000000", "cpus=8", H.header("cpufreq=1 soc_smem=0x86000000"), tag="pos")
    check("(the comparison can tell) the same words built in are none", all(r[4] for r in c), c)
    # the marker stays in the compiled object, next to the words it vouches for
    strings = subprocess.run(["strings", "-a", on], capture_output=True, text=True).stdout.split("\n")
    sha = H.header(words).split('"')[1]
    check("the marker is a string of its own in the compiled code: %s" % sha, sha in strings)
    check("no marker without the words", not any(l.startswith("hakios_builtin_args_sha=") for l in subprocess.run(["strings", "-a", off], capture_output=True, text=True).stdout.split("\n")))

    # the header the real configuration makes: what is in it
    real = H.config_py("builtin", "--header")
    text = real.stdout
    check("hakios-config.py builtin --header runs", real.returncode == 0, real.stderr)
    for need in ("soc_cpufreq=", "soc_cpufreq_step=", "soc_cpufreq_groups=", "soc_cpufreq_osm=", "soc_smem=", "thermal_soc=", "buttons_pon=",
                 "buttons_volup=", "buttons_voldown=", "cpufreq_gov=", "wifi_mac="):
        check("the real built-in words hold %s" % need, need in text)
    names = set(H.names_of(H.builtin_words()))
    check("the real built-in words hold no argument the loader or user space reads", not names & {"bootcpu", "cpus", "torch", "ufs", "wifi", "ca_vbl", "gpu", "haptic"}, names)
    realexe = H.build(work, text, "real")
    r = H.query(realexe, "cpus=8", ["cpufreq", "thermal_soc", "buttons_voldown", "cpus"])
    check("the real header, compiled into the real parser, answers", r["cpufreq"][:2] == (1, 1) and r["thermal_soc"][:2] == (1, 845) and r["buttons_voldown"][:2] == (1, 0xc4) and r["cpus"][:2] == (1, 8), r)

    print()
    print("%d failed" % len(failures) if failures else "all passed")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
