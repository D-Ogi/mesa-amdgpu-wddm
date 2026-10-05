#!/usr/bin/env python3
# Copyright The Mesa 3D Graphics Library contributors
# SPDX-License-Identifier: MIT
"""Build gate for unions that overlay a scalar integer on bit-fields.

A union of this shape

    union { struct { uint64_t a:1; uint64_t b:3; }; uint64_t key; };

only works while every bit-field sits inside the bytes of "key": callers hash that one word,
compare it, and copy a whole key through it. Whether the bits fit depends on the compiler,
because a bit-field starts a new allocation unit whenever the declared type changes size. Under
the Microsoft record layout a mix of bool and unsigned members spreads the bits over several
units, the union grows, and the overlay quietly covers only the first members.

That exact defect shipped in ac_cs_clear_copy_buffer_key: MSVC made the union 24 bytes, "key"
covered 2 of its 11 bit-fields, RADV rebuilt the shader key from "key", and the buffer fill and
copy meta shaders were built for the wrong variant. Every fill wrote the destination address in
place of the clear value, and the stores went through an undefined buffer descriptor.

A static_assert on the size of such a type turns that whole class of defect into a build failure.
This gate makes the assert mandatory instead of remembered. For every union that overlays a
scalar integer on bit-fields it wants one of two things:

  * a static_assert on sizeof() of that type in the same file (for an anonymous union, of the
    named type that holds it: a wider layout grows that type as well), or
  * a line in the allow file next to this script, which carries the reason the overlay is safe
    and a fingerprint of the union's members. Change any member and the fingerprint stops
    matching, so an upstream commit that adds a differently-typed bit-field to an allowed union
    fails the build instead of passing on a stale decision.

A union whose bits provably fit needs neither: when every bit-field is declared with one type of
a known size, the overlay is at least as wide as that type, and the bit-fields still fit in the
overlay after the units are packed the way MSVC packs them (a field that does not fit the rest of
the current unit starts a new one), the overlay covers the struct on MSVC, the SysV ABI and
anything in between. The gate computes that packing and stays quiet. It is deliberately
pessimistic everywhere else: an unknown type size, a type whose width is a platform choice
(long, size_t, an enum), several declared types, or bits that do not fit all end up on the list,
because that is where the shipped defect came from.

Exit 0 when every overlay is covered, 1 when one is not, 2 on a usage or environment error.

    python bin/check_bitfield_overlay_unions.py              # the directories our ICD builds
    python bin/check_bitfield_overlay_unions.py --root src   # the whole tree
    python bin/check_bitfield_overlay_unions.py --list       # print every overlay found
    python bin/check_bitfield_overlay_unions.py --emit-allow # allow lines for what is unprotected
"""

import argparse
import hashlib
import os
import re
import sys

# The directories that the BC-250 Vulkan ICD compiles. A new union in any of them reaches the
# shipped driver, so these are the default roots; --root takes any other subtree.
DEFAULT_ROOTS = ("src/amd", "src/vulkan", "src/compiler", "src/util")
SUFFIXES = (".h", ".hpp", ".c", ".cpp", ".cc")
ALLOW_FILE = "bitfield-overlay-unions-allow.txt"

# Types whose width is the same on every ABI we build for. "long", "size_t", "uintptr_t" and every
# enum are left out on purpose: their width is a platform choice, which is exactly what this gate
# refuses to guess.
TYPE_BITS = {
    "bool": 8, "_Bool": 8, "char": 8, "signed char": 8, "unsigned char": 8,
    "int8_t": 8, "uint8_t": 8,
    "short": 16, "short int": 16, "unsigned short": 16, "unsigned short int": 16,
    "signed short": 16, "int16_t": 16, "uint16_t": 16,
    "int": 32, "signed": 32, "signed int": 32, "unsigned": 32, "unsigned int": 32,
    "int32_t": 32, "uint32_t": 32,
    "long long": 64, "long long int": 64, "unsigned long long": 64,
    "unsigned long long int": 64, "int64_t": 64, "uint64_t": 64,
}


def packed_bits(unit_bits, widths):
    """How many bits MSVC uses for these bit-fields: a field that does not fit the rest of the
    current allocation unit starts a new one. GCC and Clang never use more."""
    used, free = 0, 0
    for w in widths:
        if w > free:
            used += unit_bits
            free = unit_bits
        free -= w
    return used

# A bit-field member: "<type> name : <width>". The width tells it from a label or a ternary.
BITFIELD = re.compile(r"^[ \t]*(?:const\s+|volatile\s+|mutable\s+|_Atomic\s+)*"
                      r"(?P<type>[A-Za-z_][\w:]*(?:\s+[A-Za-z_]\w*)*?)"
                      r"\s+(?P<name>[A-Za-z_]\w*)\s*:\s*(?P<width>\d+)\s*(?:=[^;]*)?;", re.M)
# A plain scalar member that can act as the overlay. A pointer or an array is not an overlay in
# the sense of this gate: a caller cannot use it as one key word.
SCALAR = re.compile(r"^[ \t]*(?:const\s+|volatile\s+|_Atomic\s+)*"
                    r"(?P<type>(?:unsigned\s+|signed\s+|long\s+|short\s+)*"
                    r"(?:u?int(?:8|16|32|64)_t|unsigned|signed|int|long|short|char|size_t|"
                    r"uintptr_t|uint_fast\d+_t|enum\s+\w+|[A-Za-z_]\w*_t))"
                    r"\s+(?P<name>[A-Za-z_]\w*)\s*(?:=\s*[^;]+)?;", re.M)
UNION_START = re.compile(r"(?:^|[\s(])union(?:\s+(?P<tag>[A-Za-z_]\w*))?\s*\{")
# The name right after the closing brace: "} name;", "} name = {0};", "} name[4];".
TRAILING_NAME = re.compile(r"^\}\s*(?P<name>[A-Za-z_]\w*)\s*(?:\[[^\]]*\])?\s*(?:=[^;]*)?;")
TAGGED_START = re.compile(r"(?:^|[\s(])(?:struct|class|union)\s+(?P<name>[A-Za-z_]\w*)\s*"
                          r"(?:final\s*)?(?:alignas\s*\([^)]*\)\s*)?(?::[^{;]*)?\{")
ASSERT_SIZEOF = re.compile(r"(?:static_assert|STATIC_ASSERT|_Static_assert)\s*\(\s*sizeof\s*\(\s*"
                           r"(?:struct\s+|union\s+|enum\s+)?(?P<name>[\w:]+)")
# "sizeof(((struct T *)NULL)->member)" pins a member's size, which is how RADV asserts the shader
# key it holds inside its own meta key.
ASSERT_MEMBER = re.compile(r"(?:static_assert|STATIC_ASSERT|_Static_assert)\s*\([^;]*?"
                           r"sizeof\s*\(\s*\(\s*\(\s*(?:struct\s+|union\s+)?(?P<name>[\w:]+)\s*\*")


def strip_comments(text):
    """Blanks comments and string literals, keeping every newline, so line numbers stay true."""
    out = []
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        two = text[i:i + 2]
        if two == "/*":
            end = text.find("*/", i + 2)
            end = n if end < 0 else end + 2
            out.append(re.sub(r"[^\n]", " ", text[i:end]))
            i = end
        elif two == "//":
            end = text.find("\n", i)
            end = n if end < 0 else end
            out.append(" " * (end - i))
            i = end
        elif c in "\"'":
            j = i + 1
            while j < n and text[j] != c:
                j += 2 if text[j] == "\\" else 1
            j = min(j + 1, n)
            out.append(re.sub(r"[^\n]", " ", text[i:j]))
            i = j
        else:
            out.append(c)
            i += 1
    return "".join(out)


def match_brace(text, after_open):
    """The index just past the brace that the one before "after_open" opened."""
    depth, i, n = 1, after_open, len(text)
    while i < n and depth:
        if text[i] == "{":
            depth += 1
        elif text[i] == "}":
            depth -= 1
        i += 1
    return i


def depth_zero_members(body):
    """The union's own members, with every nested struct or union blanked out."""
    out, depth = [], 0
    for ch in body:
        if ch == "{":
            depth += 1
            out.append("\n")
        elif ch == "}":
            depth -= 1
            out.append("\n")
        elif depth == 0:
            out.append(ch)
    return "".join(out)


class Overlay:
    def __init__(self, path, line, type_name, overlay, overlay_type, members):
        self.path = path.replace(os.sep, "/")
        self.line = line
        self.type_name = type_name            # the name an assert can use, or None
        self.overlay = overlay                # the scalar member that aliases the bits
        self.overlay_type = overlay_type
        self.members = members                # [(declared type, width)], in order
        self.asserted = False

    @property
    def types(self):
        return sorted({t for t, _ in self.members})

    @property
    def uniform(self):
        return len(self.types) == 1

    def fits(self):
        """Whether the overlay provably covers every bit-field on every ABI we build for.
        Returns (True, "") or (False, the reason it cannot be proved).

        MSVC packs adjacent bit-fields into one allocation unit while the declared types have the
        same size, and starts a new unit where that size changes, so the test is on the widths of
        the declared types, not on their names."""
        unknown = [t for t in self.types if t not in TYPE_BITS]
        if unknown:
            return False, "bit-field type %s has no fixed width on every ABI" % unknown[0]
        units = {TYPE_BITS[t] for t in self.types}
        if len(units) > 1:
            return False, "the declared bit-field types have different widths (%s): a new " \
                          "allocation unit starts where the width changes" % ", ".join(self.types)
        unit = units.pop()
        over = TYPE_BITS.get(self.overlay_type)
        if over is None:
            return False, "overlay type %s has no fixed width on every ABI" % self.overlay_type
        if over < unit:
            return False, "the overlay %s is narrower than the %s allocation unit" \
                          % (self.overlay_type, self.types[0])
        used = packed_bits(unit, [int(w) for _, w in self.members])
        if used > over:
            return False, "%d bit(s) of %s do not fit the %d-bit overlay" \
                          % (used, self.types[0], over)
        return True, ""

    def key(self):
        return "%s:%s:%s:%s" % (self.path, self.type_name or "<anonymous>", self.overlay,
                                self.members[0][1])

    def fingerprint(self):
        text = "|".join("%s:%s" % (t, w) for t, w in self.members) + "||" + self.overlay_type
        return hashlib.sha256(text.encode("utf-8")).hexdigest()[:12]


def enclosing_name(clean, position):
    """The named type that holds the union at "position", or the typedef name that closes it."""
    best = None
    for m in TAGGED_START.finditer(clean, 0, position):
        if match_brace(clean, m.end()) > position:
            best = m.group("name")
    if best:
        return best
    depth, i = 0, position
    while i >= 0:
        if clean[i] == "}":
            depth += 1
        elif clean[i] == "{":
            if depth == 0:
                break
            depth -= 1
        i -= 1
    if i < 0:
        return None
    end = match_brace(clean, i + 1)
    m = re.match(r"\s*(?P<name>[A-Za-z_]\w*)\s*;", clean[end:end + 200])
    return m.group("name") if m else None


def scan_text(path, text):
    """Every union in one file that overlays a scalar integer on bit-field members."""
    clean = strip_comments(text)
    found = []
    for m in UNION_START.finditer(clean):
        end = match_brace(clean, m.end())
        body = clean[m.end():end - 1]
        members = [(re.sub(r"\s+", " ", b.group("type")).strip(), b.group("width"))
                   for b in BITFIELD.finditer(body)]
        if not members:
            continue
        names = {b.group("name") for b in BITFIELD.finditer(body)}
        overlay = None
        for s in SCALAR.finditer(depth_zero_members(body)):
            if s.group("name") not in names:
                overlay = (s.group("name"), re.sub(r"\s+", " ", s.group("type")).strip())
                break
        if not overlay:
            continue
        name = m.group("tag")
        if not name:
            tail = TRAILING_NAME.match(clean[end - 1:end + 200])
            name = tail.group("name") if tail else enclosing_name(clean, m.start())
        found.append(Overlay(path, clean.count("\n", 0, m.start()) + 1, name,
                             overlay[0], overlay[1], members))
    return found


def asserted_names(text):
    clean = strip_comments(text)
    return ({m.group("name") for m in ASSERT_SIZEOF.finditer(clean)} |
            {m.group("name") for m in ASSERT_MEMBER.finditer(clean)})


def read_allow(path):
    """key -> (fingerprint, reason). A line is "<key> <fingerprint>  # why it is safe"."""
    allow = {}
    if not os.path.exists(path):
        return allow
    with open(path, "r", encoding="utf-8") as f:
        for lineno, raw in enumerate(f, 1):
            line = raw.strip()
            if not line or line.startswith("#"):
                continue
            entry, _, reason = line.partition("#")
            fields = entry.split()
            if len(fields) != 2:
                raise SystemExit("%s:%d: a line is \"<key> <fingerprint>  # reason\", got %r"
                                 % (path, lineno, line))
            allow[fields[0]] = (fields[1], reason.strip())
    return allow


def collect(tree, roots):
    overlays, files = [], 0
    for root in roots:
        base = os.path.join(tree, root.replace("/", os.sep))
        if not os.path.isdir(base):
            raise SystemExit("check_bitfield_overlay_unions: no such directory: %s" % base)
        for dirpath, dirnames, filenames in os.walk(base):
            dirnames[:] = [d for d in dirnames if d not in (".git", "__pycache__")]
            for fn in sorted(filenames):
                if not fn.endswith(SUFFIXES):
                    continue
                path = os.path.join(dirpath, fn)
                with open(path, "r", encoding="utf-8", errors="replace") as f:
                    text = f.read()
                files += 1
                if "union" not in text:
                    continue
                here = scan_text(os.path.relpath(path, tree), text)
                if not here:
                    continue
                names = asserted_names(text)
                for ov in here:
                    ov.asserted = bool(ov.type_name) and ov.type_name in names
                    overlays.append(ov)
    return files, overlays


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--root", action="append", default=[],
                    help="a subtree to scan, relative to the source tree (default: %s)"
                         % ", ".join(DEFAULT_ROOTS))
    ap.add_argument("--tree", default=os.path.dirname(here),
                    help="the source tree (default: the one that holds bin/)")
    ap.add_argument("--allow", default=os.path.join(here, ALLOW_FILE))
    ap.add_argument("--list", action="store_true", help="print every overlay, allowed or not")
    ap.add_argument("--emit-allow", action="store_true",
                    help="print allow lines for the unprotected overlays and stop")
    args = ap.parse_args()

    allow = read_allow(args.allow)
    files, overlays = collect(args.tree, args.root or list(DEFAULT_ROOTS))
    try:
        allow_rel = os.path.relpath(args.allow, args.tree).replace(os.sep, "/")
    except ValueError:  # another drive on Windows: the absolute path is the only useful name
        allow_rel = args.allow

    missing, changed, allowed, fitting = [], [], [], []
    for ov in overlays:
        if ov.asserted:
            continue
        ok, ov.why = ov.fits()
        if ok:
            fitting.append(ov)
            continue
        entry = allow.get(ov.key())
        if entry is None:
            missing.append(ov)
        elif entry[0] != ov.fingerprint():
            changed.append(ov)
        else:
            allowed.append(ov)

    if args.emit_allow:
        seen = set()
        for ov in sorted(missing + changed, key=lambda o: o.key()):
            if ov.key() in seen:
                continue
            seen.add(ov.key())
            print("%s %s  # %s" % (ov.key(), ov.fingerprint(), ov.why))
        return 0

    if args.list:
        for ov in sorted(overlays, key=lambda o: o.key()):
            state = ("assert" if ov.asserted else "fits" if ov in fitting else
                     "MISSING" if ov in missing else "CHANGED" if ov in changed else "allowed")
            print("%-8s %-8s %s:%d %s over %s (%s)"
                  % (state, "one" if ov.uniform else "SEVERAL", ov.path, ov.line,
                     ov.type_name or "<anonymous>", ov.overlay, ", ".join(ov.types)))

    for ov in sorted(missing, key=lambda o: o.key()):
        print("%s:%d: the union %s overlays %s on bit-fields, and the overlay is not provably\n"
              "    whole: %s.\n"
              "    Declare every bit-field of it with one type and add\n"
              "        static_assert(sizeof(%s) == <bytes>, \"<why>\");\n"
              "    next to it, or record the decision in %s:\n"
              "        %s %s  # <why the overlay covers every bit-field>"
              % (ov.path, ov.line, ov.type_name or "<anonymous>", ov.overlay, ov.why,
                 ov.type_name or "<type>", allow_rel, ov.key(), ov.fingerprint()), file=sys.stderr)
    for ov in sorted(changed, key=lambda o: o.key()):
        print("%s:%d: the union %s is allowed in %s, but its members have changed since that\n"
              "    decision (fingerprint %s, the file says %s). Re-check that %s still covers\n"
              "    every bit-field, then update the fingerprint."
              % (ov.path, ov.line, ov.type_name or "<anonymous>", allow_rel, ov.fingerprint(),
                 allow[ov.key()][0], ov.overlay), file=sys.stderr)

    print("bitfield overlay unions: %d file(s), %d overlay(s): %d asserted, %d provably whole, "
          "%d allowed, %d unprotected, %d changed since their decision"
          % (files, len(overlays), len(overlays) - len(fitting) - len(missing) - len(changed) -
             len(allowed), len(fitting), len(allowed), len(missing), len(changed)))
    return 1 if missing or changed else 0


if __name__ == "__main__":
    sys.exit(main())
