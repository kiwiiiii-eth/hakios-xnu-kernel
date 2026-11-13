"""tools/args_host.py: pexpert/gen/bootargs.c compiled on the host, for tools/args-builtin-test.py, tools/args-equiv.py and
tools/args-run-test.py. The file is the kernel's own (copied byte for byte beside the generated hakios.builtin_args.h it
includes, with stand-ins for <pexpert/pexpert.h> and <pexpert/device_tree.h>); the harness asks it, for a boot line, what
PE_parse_boot_argn and PE_parse_boot_arg_str answer."""
import atexit
import hashlib
import os
import re
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
SOURCE = os.path.join(ROOT, "xnu-src", "xnu-8796.121.2", "pexpert", "gen", "bootargs.c")
DISABLED = "#define HAKIOS_ARGS_BUILTIN 0\n"


def tmpdir(prefix):
    """A temporary directory that is removed when the test ends."""
    d = tempfile.mkdtemp(prefix=prefix)
    atexit.register(shutil.rmtree, d, True)
    return d


def header(words=None):
    """The text of hakios.builtin_args.h for built-in `words` (None: no built-in arguments), made the way hakios-config.py does."""
    if words is None:
        return DISABLED
    sha = hashlib.sha256(words.encode()).hexdigest()[:16]
    return ("#define HAKIOS_ARGS_BUILTIN 1\nconst char hakios_builtin_args_sha[] __attribute__((used)) = \"hakios_builtin_args_sha=%s\";\n"
            "static const char hakios_builtin_args[] = \"%s\";\n" % (sha, words))


def build(workdir, header_text, tag):
    """Compile the real bootargs.c with the header; returns the harness binary."""
    d = os.path.join(workdir, tag)
    os.makedirs(d, exist_ok=True)
    shutil.copyfile(SOURCE, os.path.join(d, "bootargs.c"))
    with open(SOURCE, "rb") as a, open(os.path.join(d, "bootargs.c"), "rb") as b:
        assert a.read() == b.read(), "the copy of bootargs.c is not the kernel's file"
    with open(os.path.join(d, "hakios.builtin_args.h"), "w") as f:
        f.write(header_text)
    exe = os.path.join(d, "harness")
    cmd = ["cc", "-std=gnu11", "-O1", "-w", "-I", os.path.join(HERE, "args-builtin", "stub"), "-o", exe,
           os.path.join(HERE, "args-builtin", "harness.c"), os.path.join(d, "bootargs.c")]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode:
        raise RuntimeError("compiling bootargs.c on the host failed:\n" + r.stderr[-2000:])
    return exe


def query(exe, cmdline, names):
    """{name: (found_num, num, found_str, str, found_presence)} for the boot line `cmdline` (None: an empty one)."""
    r = subprocess.run([exe, "-" if cmdline is None else cmdline], input="\n".join(names) + "\n", capture_output=True, text=True, check=True)
    out = {}
    for row in r.stdout.split("\n"):
        if row:
            f = row.split("\t")
            out[f[0]] = (int(f[1]), int(f[2]), int(f[3]), f[4], int(f[5]))
    return out


def names_of(line):
    """The argument names of a boot line, in order: `-v` stays `-v`, name=value gives name."""
    return [w.split("=", 1)[0] for w in line.split()]


def config_py(*args, root=ROOT):
    r = subprocess.run([sys.executable, "-I", os.path.join(root, "tools", "hakios-config.py"), "--root", root] + list(args),
                       capture_output=True, text=True)
    return r


def builtin_words(root=ROOT):
    r = config_py("builtin", root=root)
    if r.returncode:
        raise RuntimeError(r.stderr)
    return r.stdout.rstrip("\n")


R69 = os.path.join(HERE, "args-builtin", "r69.cmdline")      # run 69's whole boot line, as the loader logged it (597 characters)


def r69():
    return open(R69, encoding="utf-8").read()


def loader_prefix(line=None):
    """What the loader puts before the run script's own words (debug=, rd=, ufs_shared_pa=, hakios_kver=): up to loader_return_after="""
    line = line or r69()
    return line[:line.index("loader_return_after=")]


def dryrun(script, kernel, k=ROOT, secs="300", extra=" ipa8=10", device_repo=None, env=None):
    """Run one of the device repository's run scripts in its DRYRUN_CMDLINE mode (it prints the command line and stops before
    fastboot, USB or the phone) with the kernel repository `k` and the kernel file `kernel`. A fastboot, adb and edl that fail and
    leave a note stand first in PATH: a script that reached for the phone would be caught. Returns
    (returncode, stdout, stderr, the script's command line or None, whether a phone tool was called)."""
    work = tmpdir("args-dryrun-")
    root = os.path.join(work, "root")
    os.makedirs(os.path.join(root, "out"))
    os.symlink(os.path.abspath(k), os.path.join(root, "hakios-xnu-kernel"))
    bindir = os.path.join(work, "bin")
    os.makedirs(bindir)
    note = os.path.join(work, "phone-tool-called")
    for tool in ("fastboot", "adb", "edl", "edl.py"):
        p = os.path.join(bindir, tool)
        open(p, "w").write("#!/bin/sh\necho %s >> %s\nexit 99\n" % (tool, note))
        os.chmod(p, 0o755)
    device_repo = device_repo or os.path.join(ROOT, "devices", "fajita")
    path = os.path.join(device_repo, "scripts", script)
    e = dict(os.environ, PATH=bindir + os.pathsep + os.environ["PATH"], HAKIOS_ROOT=root, DRYRUN_CMDLINE="1", SECS=secs)
    e.pop("KERNEL", None)
    e.update(env or {})
    if script == "run-fajita.sh":
        argv = ["bash", path, kernel, "none", extra]
    else:
        e["KERNEL"] = kernel
        argv = ["bash", path, "none", "none", extra]
    r = subprocess.run(argv, capture_output=True, text=True, errors="replace", env=e, timeout=120)
    line = None
    for row in r.stdout.split("\n"):
        m = re.match(r"(run|darwin): cmdline=(.*)$", row)
        if m:
            line = m.group(2)
    return r.returncode, r.stdout, r.stderr, line, os.path.exists(note)


def normalize_epoch(line, to="1794567272"):
    """The run script's epoch= is the time it ran: set it to run 69's so lines can be compared."""
    return re.sub(r"epoch=\d+", "epoch=" + to, line)


def show(r):
    """One answer of query() as a table cell."""
    num, val, fstr, text, pres = r
    if not (num or fstr or pres):
        return "(not set)"
    return text if fstr else ("%d" % val)


def compare(work, before_cmd, after_cmd, after_header, extra_names=(), tag=""):
    """What the kernel reads from `before_cmd` (no built-in words) and from `after_cmd` with the built-in header `after_header`,
    for every name on either line and in `extra_names`: [(name, before, after, where the after answer comes from, same)]."""
    exe_before = build(work, header(None), "before" + tag)
    exe_after = build(work, after_header, "after" + tag)
    names = []
    for n in names_of(before_cmd) + names_of(after_cmd) + list(extra_names):
        if n not in names:
            names.append(n)
    qb, qa = query(exe_before, before_cmd, names), query(exe_after, after_cmd, names)
    on_line = set(names_of(after_cmd))
    return [(n, show(qb[n]), show(qa[n]), "boot line" if n in on_line else "kernel", qb[n] == qa[n]) for n in names]


def make_k(work, name, old_tool_ref=None, builtin=True):
    """A temporary kernel repository for the run scripts: this repository's configs and drivers by links, a .config of the 6T
    (with or without CONFIG_ARGS_BUILTIN), devices/fajita as the device repository, and the tool of this repository (links) or the
    older one of `old_tool_ref` (a commit before `builtin` and `--full` existed). Returns its path."""
    t = os.path.join(work, name)
    os.makedirs(os.path.join(t, "devices"))
    for n in ("configs", "drivers", "socs", "VERSION", "docs"):
        if os.path.exists(os.path.join(ROOT, n)):
            os.symlink(os.path.join(ROOT, n), os.path.join(t, n))
    for n in os.listdir(os.path.join(ROOT, "devices")):
        os.symlink(os.path.realpath(os.path.join(ROOT, "devices", n)), os.path.join(t, "devices", n))
    if old_tool_ref:
        os.makedirs(os.path.join(t, "tools"))
        src = subprocess.run(["git", "-C", ROOT, "show", "%s:tools/hakios-config.py" % old_tool_ref], capture_output=True, text=True, check=True).stdout
        open(os.path.join(t, "tools", "hakios-config.py"), "w").write(src)
    else:
        os.symlink(os.path.join(ROOT, "tools"), os.path.join(t, "tools"))
    r = subprocess.run([sys.executable, "-I", os.path.join(ROOT, "tools", "hakios-config.py"), "--root", ROOT, "defconfig", "fajita", "-o",
                        os.path.join(t, ".config.full")], capture_output=True, text=True, check=True)
    text = open(os.path.join(t, ".config.full")).read()
    os.remove(os.path.join(t, ".config.full"))
    if not builtin:
        text = "".join(l for l in text.splitlines(True) if "ARGS_BUILTIN" not in l)
    open(os.path.join(t, ".config"), "w").write(text)
    return t
