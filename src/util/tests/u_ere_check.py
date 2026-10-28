# SPDX-License-Identifier: MIT
"""Host test of u_ere (the driconf regex matcher of Windows builds) against Python's re as the oracle.

    python -I src/util/tests/u_ere_check.py <mesa tree> <u_ere_test.exe>   (build: u_ere_test.c)

1. Every regular expression of every driconf file of the tree compiles, and gives the same result as the oracle
   for a generated matching subject, for mutations of it and for the engine and executable names below.
2. Section table: for the engine names of vkd3d, DXVK, zink and Unreal Engine and for a list of Windows game
   executables, the driconf sections that match are exactly the expected ones.
3. Syntax cases (supported and refused constructs) and a random differential test.
Exit 0 when every check passes.
"""
import glob
import os
import random
import re
import subprocess
import sys
import xml.etree.ElementTree as ET

try:
    import re._parser as sre_parse
    import re._constants as sre_c
except ImportError:  # Python < 3.11
    import sre_parse
    import sre_constants as sre_c

tree, driver = sys.argv[1], sys.argv[2]
ATTRS = ("engine_name_match", "executable_regexp", "application_name_match", "device_version_regexp")
failures = []


def run(cases):
    """cases: (pattern bytes, subject bytes, cflags) -> [(comp, exec)]"""
    hx = lambda b: b.hex() if b else "-"
    inp = "".join(f"{hx(p)} {hx(s)} {f}\n" for p, s, f in cases)
    out = subprocess.run([driver], input=inp, capture_output=True, text=True, check=True).stdout.split("\n")
    return [tuple(int(x) for x in line.split()) for line in out if line.strip()]


CLASSES = {
    "alnum": "a-zA-Z0-9", "alpha": "a-zA-Z", "blank": " \\t", "cntrl": "\\x01-\\x1f\\x7f", "digit": "0-9",
    "graph": "!-~", "lower": "a-z", "print": " -~", "punct": "!-/:-@\\[-`{-~", "space": " \\t\\n\\r\\f\\v",
    "upper": "A-Z", "xdigit": "0-9A-Fa-f",
}


def to_python(p):
    """A POSIX ERE (the u_ere subset) as a Python bytes regex with the same match result."""
    out, i, n = [], 0, len(p)
    while i < n:
        c = p[i]
        if c == "\\":
            out.append(re.escape(p[i + 1]))
            i += 2
        elif c == "$":
            out.append(r"\Z")
            i += 1
        elif c == "[":
            j = i + 1
            body = ["["]
            if j < n and p[j] == "^":
                body.append("^")
                j += 1
            first = True
            while True:
                if p[j] == "]" and not first:
                    break
                first = False
                if p.startswith("[:", j):
                    k = p.index(":]", j)
                    body.append(CLASSES[p[j + 2:k]])
                    j = k + 2
                    continue
                ch = p[j]
                body.append("\\" + ch if ch in "\\[]^-" else ch)
                if j + 2 < n and p[j + 1] == "-" and p[j + 2] != "]":
                    hi = p[j + 2]
                    body.append("-" + ("\\" + hi if hi in "\\[]^-" else hi))
                    j += 3
                else:
                    j += 1
            body.append("]")
            out.append("".join(body))
            i = j + 1
        else:
            out.append(c)
            i += 1
    return re.compile("".join(out).encode("latin-1"), re.DOTALL)


def oracle(p, s):
    return 0 if to_python(p).search(s.encode("latin-1")) else 1


def sample(p, rnd):
    """A subject that the pattern matches, from Python's parse of it."""
    def gen(items):
        s = ""
        for op, av in items:
            if op is sre_c.LITERAL:
                s += chr(av)
            elif op is sre_c.ANY:
                s += rnd.choice("xY7.")
            elif op is sre_c.IN:
                neg = any(o is sre_c.NEGATE for o, _ in av)
                chars = set()
                for o, a in av:
                    if o is sre_c.LITERAL:
                        chars.add(chr(a))
                    elif o is sre_c.RANGE:
                        chars.update(chr(x) for x in range(a[0], a[1] + 1))
                if neg:
                    chars = set(chr(x) for x in range(32, 127)) - chars
                s += rnd.choice(sorted(chars))
            elif op in (sre_c.MAX_REPEAT, sre_c.MIN_REPEAT):
                lo, hi, sub = av
                hi = min(hi, lo + 2)
                s += "".join(gen(sub) for _ in range(rnd.randint(lo, hi)))
            elif op is sre_c.SUBPATTERN:
                s += gen(av[-1])
            elif op is sre_c.BRANCH:
                s += gen(rnd.choice(av[1]))
            elif op is sre_c.AT:
                pass
            else:
                raise ValueError(op)
        return s
    return gen(sre_parse.parse(to_python(p).pattern.decode("latin-1")))


def mutations(s, rnd):
    out = {s, s + "x", "x" + s, s.upper(), s.lower(), s[1:], s[:-1], " " + s + " ", s + "\n"}
    if s:
        k = rnd.randrange(len(s))
        out.add(s[:k] + s[k + 1:])
        out.add(s[:k] + "#" + s[k + 1:])
    return out


# The names that the section table checks, and the subjects that every pattern also meets.
ENGINES = ["vkd3d", "DXVK", "DXVK_NvRemix", "mesa zink", "UnrealEngine4.27", "UnrealEngine5", "UnrealEngine5.3",
           "Quantic Dream Engine", "GTK", ""]
LAB_EXES = ["witcher3.exe", "ROTTR.exe", "SOTTR.exe", "q2rtx.exe", "TheAscent-Win64-Shipping.exe", "factorio.exe",
            "3DMarkTimeSpy.exe", "3DMarkPortRoyal.exe", "cs2.exe", "aces.exe", "Heaven.exe", "Cyberpunk2077.exe",
            "NMS.exe", "dwm.exe", "explorer.exe"]


def driconf_patterns():
    pats = []
    for f in sorted(glob.glob(os.path.join(tree, "src", "**", "*.conf"), recursive=True)):
        try:
            root = ET.parse(f).getroot()
        except ET.ParseError:
            continue
        if root.tag != "driconf":
            continue
        for el in root.iter():
            for a in ATTRS:
                if a in el.attrib:
                    pats.append((os.path.relpath(f, tree).replace("\\", "/"), el.tag, el.attrib.get("name", ""), a,
                                 el.attrib[a]))
    return pats


rnd = random.Random(180)
pats = driconf_patterns()
uniq = sorted({p[4] for p in pats})

# 1. Every driconf pattern.
cases, expect = [], []
for p in uniq:
    subjects = set(ENGINES) | set(LAB_EXES)
    for _ in range(4):
        subjects |= mutations(sample(p, rnd), rnd)
    for s in sorted(subjects):
        cases.append((p.encode(), s.encode("latin-1"), 1))
        expect.append((p, s, oracle(p, s)))
res = run(cases)
npos = 0
for (p, s, want), (c, e) in zip(expect, res):
    npos += want == 0
    if c != 0:
        failures.append(f"driconf pattern does not compile: {p!r} ({c})")
    elif e != want:
        failures.append(f"driconf pattern {p!r} on {s!r}: u_ere {e}, oracle {want}")
print(f"driconf: {len(pats)} attributes, {len(uniq)} distinct patterns, {len(cases)} subjects "
      f"({npos} matching), from {len({p[0] for p in pats})} files")

# 2. Section table: which sections apply to each engine name and game executable.
EXPECT_ENGINE = {
    "vkd3d": {"vkd3d", "DXVK|vkd3d", "vkd3d|DXVK"},
    "DXVK": {"DXVK", "DXVK|vkd3d", "vkd3d|DXVK"},
    "DXVK_NvRemix": {"DXVK", "DXVK_NvRemix", "DXVK|vkd3d", "vkd3d|DXVK"},
    "mesa zink": {"mesa zink"},
    "UnrealEngine4.27": {"^UnrealEngine", "UnrealEngine4.*"},
    "UnrealEngine5": {"^UnrealEngine", "UnrealEngine5.*"},
    "UnrealEngine5.3": {"^UnrealEngine", "UnrealEngine5.*"},
    "Quantic Dream Engine": {"Quantic Dream Engine"},
    "GTK": {"GTK"},
    "": set(),
}
EXPECT_APP = {"SOTTR.exe": {"SOTTR.exe"}}
eng_pats = sorted({p[4] for p in pats if p[3] == "engine_name_match" and not p[0].startswith("src/util/tests/")})
app_pats = sorted({p[4] for p in pats if p[3] in ("executable_regexp", "application_name_match")
                   and not p[0].startswith("src/util/tests/")})
cases = [(p.encode(), e.encode(), 1) for e in EXPECT_ENGINE for p in eng_pats]
cases += [(p.encode(), x.encode(), 1) for x in LAB_EXES for p in app_pats]
res = iter(run(cases))
for e, want in EXPECT_ENGINE.items():
    got = {p for p in eng_pats if next(res)[1] == 0}
    want = want & set(eng_pats) | (want - set(eng_pats))
    if got != want:
        failures.append(f"engine {e!r}: sections {sorted(got)}, expected {sorted(want)}")
    print(f"engine {e!r:24} -> {sorted(got)}")
for x in LAB_EXES:
    got = {p for p in app_pats if next(res)[1] == 0}
    want = EXPECT_APP.get(x, set())
    if got != want:
        failures.append(f"executable {x!r}: sections {sorted(got)}, expected {sorted(want)}")
    print(f"executable {x!r:32} -> {sorted(got)}")

# 3a. Syntax: (pattern, subject, expected compile result, expected match or None).
SYNTAX = [
    ("abc", "xabcx", 0, 0), ("^abc", "xabc", 0, 1), ("abc$", "abcx", 0, 1), ("abc$", "abc\n", 0, 1),
    ("^$", "", 0, 0), ("a|b", "b", 0, 0), ("(ab)+c", "ababc", 0, 0), ("(ab){2}c", "abc", 0, 1),
    ("a{2,3}", "aa", 0, 0), ("^a{2,3}$", "aaaa", 0, 1), ("^a{2,}$", "aaaaaa", 0, 0), ("x?y", "y", 0, 0),
    ("[[:digit:]]+v", "12v", 0, 0), ("[^a-c]", "abc", 0, 1), ("[]a]", "]", 0, 0), ("[a-]", "-", 0, 0),
    ("a.c", "a\nc", 0, 0), ("\\.exe$", "a.exe", 0, 0), ("\\.exe$", "aexe", 0, 1), ("()", "", 0, 0),
    ("(|a)b", "b", 0, 0), ("(a*)*b", "aaab", 0, 0), ("(a|)+$", "aaa", 0, 0), ("ABC", "abc", 0, 1),
    # refused constructs
    ("*a", "", 2, None), ("a**", "", 0, 0), ("(a", "", 2, None), ("a)", "", 2, None), ("[a", "", 2, None),
    ("a{", "", 2, None), ("a{,2}", "", 2, None), ("a{3,2}", "", 2, None), ("a{256}", "", 12, None),
    ("\\1", "", 2, None), ("\\w", "", 2, None), ("[[.a.]]", "", 2, None), ("[[:nope:]]", "", 2, None),
    ("^*", "", 2, None), ("[z-a]", "", 2, None), ("a\\", "", 2, None),
]
res = run([(p.encode(), s.encode(), 1) for p, s, _, _ in SYNTAX])
for (p, s, wc, we), (c, e) in zip(SYNTAX, res):
    if c != wc or (we is not None and e != we):
        failures.append(f"syntax {p!r} on {s!r}: u_ere ({c}, {e}), expected ({wc}, {we})")
# Case-insensitive compile flag (2).
res = run([(b"aB[c-d]", b"xAbDx", 2), (b"aB[c-d]", b"xAbDx", 1)])
if res != [(0, 0), (0, 1)]:
    failures.append(f"icase: {res}")
print(f"syntax: {len(SYNTAX) + 2} cases")

# 3b. Random differential test against the oracle.
ALPH = "ab."


def rnd_pattern(depth=0):
    parts = []
    for _ in range(rnd.randint(1, 3)):
        k = rnd.random()
        if k < 0.45:
            atom = rnd.choice("abc")
        elif k < 0.55:
            atom = "."
        elif k < 0.7:
            atom = "[" + rnd.choice(["", "^"]) + rnd.choice(["ab", "a-c", "b", "^a", "a-b]"[:3]]) + "]"
        elif k < 0.8 and depth < 3:
            atom = "(" + rnd_pattern(depth + 1) + ")"
        elif k < 0.85:
            atom = "\\."
        else:
            atom = rnd.choice("ab")
        q = rnd.random()
        if q < 0.15:
            atom += "*"
        elif q < 0.25:
            atom += "+"
        elif q < 0.35:
            atom += "?"
        elif q < 0.42:
            lo = rnd.randint(0, 2)
            atom += rnd.choice([f"{{{lo}}}", f"{{{lo},}}", f"{{{lo},{lo + rnd.randint(0, 2)}}}"])
        parts.append(atom)
    p = "".join(parts)
    if rnd.random() < 0.2:
        p = p + "|" + rnd_pattern(depth + 1) if depth < 3 else p
    if depth == 0:
        if rnd.random() < 0.3:
            p = "^" + p
        if rnd.random() < 0.3:
            p = p + "$"
    return p


cases, expect = [], []
for _ in range(4000):
    p = rnd_pattern()
    for _ in range(6):
        s = "".join(rnd.choice("abc.\n") for _ in range(rnd.randint(0, 8)))
        cases.append((p.encode(), s.encode(), 1))
        expect.append((p, s, oracle(p, s)))
res = run(cases)
diff = 0
for (p, s, want), (c, e) in zip(expect, res):
    if c != 0 or e != want:
        diff += 1
        if diff <= 20:
            failures.append(f"random {p!r} on {s!r}: u_ere ({c}, {e}), oracle {want}")
print(f"random: {len(cases)} cases, {diff} differences")

for f in failures:
    print("FAIL", f)
print("PASS" if not failures else f"FAIL ({len(failures)})")
sys.exit(1 if failures else 0)
