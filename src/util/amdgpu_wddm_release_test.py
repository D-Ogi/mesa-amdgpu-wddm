"""BD-167: the amdgpu-wddm release flavour of Mesa compiles no lookup of a forbidden override variable.

    python src/util/amdgpu_wddm_release_test.py [--root <mesa source tree>]

The release flavour is the default build. Only a named lab build passes -Damdgpu-wddm-lab-flavour=true, which
adds -DAMDGPU_WDDM_LAB_FLAVOUR=1 to pre_args (every C and C++ unit), so a build that forgets the switch is the
release flavour. A release driver must not let an environment variable replace, capture or dump shader code,
move or add to the cache of compiled shader code, read configuration from another directory, pick another or a
software driver, write driver traces or log files, or switch on debug, test or experimental code paths, and the
criterion is that
the variable's name is absent from the binary: a lookup
that is compiled out passes, a lookup that is only ignored at run time does not.

The test reads every C and C++ file under src/ except the trees no amdgpu-wddm build compiles (SKIP), blanks the
comments, resolves only the conditionals on AMDGPU_WDDM_LAB_FLAVOUR (any other conditional keeps both branches,
so a name behind it still counts) and collects the string literals that hold a forbidden name as a whole word:
  - release (the macro not defined): no literal may hold one;
  - lab (the macro defined): every forbidden name is still found, which shows that the scan reaches its lookup
    and that the lab build keeps it;
  - the option exists, is boolean with the value false, and meson.build adds -DAMDGPU_WDDM_LAB_FLAVOUR=1 for it.
The bc250-win gate tools/quality/flavour_gate.py checks the same names in the built DLLs.
"""
import argparse
import os
import re
import sys

FORBIDDEN = (
    'MESA_SPIRV_READ_PATH',          # replaces each SPIR-V module with a file of the same hash from a directory
    'MESA_SHADER_READ_PATH',         # replaces GLSL source with a file of the same hash from a directory
    'MESA_SHADER_CAPTURE_PATH',      # writes the source of every linked GL program to a directory
    'MESA_LOADER_DRIVER_OVERRIDE',   # names another driver than the built-in choice
    'GALLIUM_DRIVER',                # selects the gallium driver in place of the built-in choice
    'RADV_DEBUG',                    # RADV debug flags: code generation, caching, synchronisation, dumps
    'RADV_PERFTEST',                 # RADV experimental code paths
    'ACO_DEBUG',                     # ACO compiler debug flags: validation, optimisation, scheduling
    'ZINK_DEBUG',                    # zink debug flags: validation, optimisation, synchronisation, dumps
    'MESA_SPIRV_DUMP_PATH',          # writes every SPIR-V module to a directory
    'MESA_SPIRV_FAIL_DUMP_PATH',     # writes each SPIR-V module the compiler refused to a directory
    'MESA_SHADER_DUMP_PATH',         # writes the source of every GLSL shader to a directory
    'MESA_SHADER_CACHE_DIR',         # moves the disk cache of compiled shader code to a directory
    'MESA_GLSL_CACHE_DIR',           # the deprecated name of MESA_SHADER_CACHE_DIR
    'DRIRC_CONFIGDIR',               # reads driver configuration files from a directory in place of the built-in one
    'XDG_CACHE_HOME',                # moves the disk cache of compiled shader code under another home directory
    'MESA_DISK_CACHE_READ_ONLY_FOZ_DBS', # loads compiled shader code from read-only cache databases
    'MESA_DISK_CACHE_READ_ONLY_FOZ_DBS_DYNAMIC_LIST', # a file that names more read-only cache databases
    'RADV_EXPERIMENTAL',             # RADV experimental features: sparse binding, video, emulated ray tracing, ELF
    'MESA_VK_TRACE',                 # driver traces (RGP, RRA, RMV, context rolls) written to files
    'MESA_VK_TRACE_PER_SUBMIT',      # a driver trace per queue submission
    'MESA_VK_TRACE_FRAME',           # the frame a driver trace starts at
    'MESA_VK_TRACE_TRIGGER',         # a file whose existence starts a driver trace
    'LIBGL_ALWAYS_SOFTWARE',         # selects a software renderer in place of the GPU
    'MESA_DRICONF_EXECUTABLE_OVERRIDE',  # applies the driver configuration of another executable
    'RADV_TRAP_HANDLER_EXCP',        # arms the shader trap handler for memory and float exceptions
    'RADV_PROFILE_PSTATE',           # the power state RADV asks for while a profiler runs
    'RADV_QUEUE_DISABLE',            # hides graphics, compute, video, transfer or sparse queues
    'RADV_FORCE_VRS_CONFIG_FILE',    # reads the forced shading rate from a file
    'RADV_BVH_STATS_FILE',           # writes acceleration structure statistics to a file
    'RADV_FORCE_VRS',                # forces a variable shading rate on every draw (upstream: for testing)
    'RADV_TEX_ANISO',                # forces an anisotropy filter on every sampler; no release consumer
    'MESA_LOG_FILE',                 # writes the Mesa log to a file (not compiled on Windows)
    'MESA_LOG_FILE_AUTO',            # writes the Mesa log to a file of its own (not compiled on Windows)
    'GALLIUM_LOG_FILE',              # writes the gallium log to a file (debug builds only)
    'D3D_ALWAYS_SOFTWARE',           # makes zink pick a software Vulkan device in place of the GPU
    # The amdgpu-wddm winsys: bisect and diagnostic switches. The release flavour takes the value of an unset
    # variable.
    'BC250_IB_DWORDS',               # cuts the IB the CP executes
    'BC250_TRACE_SUBMITS',           # writes a line for every submission
    'AMDGPU_WDDM_PROFILE_DIR',       # reads the run-time driver configuration files from another directory
)
SCAN = ('src',)
EXTENSIONS = ('.c', '.cc', '.cpp', '.h', '.hpp')
SKIP = (
    (r'^src/egl/', 'EGL: every amdgpu-wddm recipe passes -Degl=disabled'),
    (r'^src/glx/', 'GLX: every amdgpu-wddm recipe passes -Dglx=disabled'),
    (r'^src/amd/vulkan/winsys/amdgpu/', 'the Linux winsys: src/amd/vulkan/meson.build leaves it out on Windows'),
    (r'(^|/)tests?/', 'test programs: no package ships them'),
)
MESON_OPTIONS = 'meson.options'
DEFAULT_ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..'))
class Unsupported(Exception):
    pass


LITERAL = re.compile(r'"(?:[^"\\\n]|\\.)*"')
CHAR = re.compile(r"'(?:\\.|[^\\'\n]){1,4}'")
DIRECTIVE = re.compile(r'^\s*#\s*(if|ifdef|ifndef|elif|else|endif)\b(.*)$')
MACRO = 'AMDGPU_WDDM_LAB_FLAVOUR'
OPTION = 'amdgpu-wddm-lab-flavour'


def strip_comments(text):
    """The text with comments blanked and the line structure kept; string and character literals stay."""
    out, i, n = [], 0, len(text)
    while i < n:
        c = text[i]
        if text.startswith('/*', i):
            j = text.find('*/', i + 2)
            j = n if j < 0 else j + 2
            out.append('\n' * text.count('\n', i, j))
            i = j
        elif text.startswith('//', i):
            j = text.find('\n', i)
            i = n if j < 0 else j
        elif c == '"':
            m = LITERAL.match(text, i)
            out.append(m.group() if m else c)
            i = m.end() if m else i + 1
        elif c == "'":
            m = CHAR.match(text, i)
            out.append(m.group() if m else c)
            i = m.end() if m else i + 1
        else:
            out.append(c)
            i += 1
    return ''.join(out)


def lab_condition(expr):
    """True/False when the expression is exactly 'the lab macro is defined' or its negation, None when it does
    not name the macro. Any other use of the macro is a form this test does not read: Unsupported."""
    e = re.sub(r'\s+', '', expr)
    if MACRO not in e:
        return None
    forms = {MACRO: True, 'defined(%s)' % MACRO: True, 'defined%s' % MACRO: True}
    negated = e.startswith('!')
    body = e[1:] if negated else e
    if body.startswith('(') and body.endswith(')') and body[1:-1] in forms:
        body = body[1:-1]
    if body not in forms:
        raise Unsupported(expr.strip())
    return not negated


def active_lines(text, lab):
    """For each line, whether the build of this flavour compiles it. Conditionals on other macros keep both
    branches active, so a name behind any of them still counts."""
    lines = text.split('\n')
    stack, result = [], []   # stack entries: None (other macro) or [wanted_now, taken]
    joined = ''
    for line in lines:
        logical = joined + line
        if logical.endswith('\\'):
            joined = logical[:-1]
            result.append(all(f is None or f[0] for f in stack))
            continue
        joined = ''
        m = DIRECTIVE.match(logical)
        if not m:
            result.append(all(f is None or f[0] for f in stack))
            continue
        kind, rest = m.group(1), m.group(2)
        if kind in ('if', 'ifdef', 'ifndef'):
            arg = rest.strip()
            cond = lab_condition(arg) if kind == 'if' else (
                None if arg.split()[0:1] != [MACRO] else (kind == 'ifdef'))
            stack.append(None if cond is None else [cond == lab, cond == lab])
        elif kind == 'elif':
            if not stack:
                raise Unsupported('#elif without #if')
            if stack[-1] is not None or lab_condition(rest) is not None:
                raise Unsupported('#elif in a conditional on ' + MACRO)
        elif kind == 'else':
            if not stack:
                raise Unsupported('#else without #if')
            if stack[-1] is not None:
                stack[-1][0] = not stack[-1][1]
        elif kind == 'endif':
            if not stack:
                raise Unsupported('#endif without #if')
            stack.pop()
        result.append(all(f is None or f[0] for f in stack))
    return result


def findings(path, text, lab):
    """(line, name) for each forbidden name in a string literal that this flavour compiles."""
    text = strip_comments(text)
    active = active_lines(text, lab)
    out = []
    for number, (line, on) in enumerate(zip(text.split('\n'), active), 1):
        if not on:
            continue
        for lit in LITERAL.finditer(line):
            for name in FORBIDDEN:
                if re.search(r'(?<![A-Za-z0-9_])%s(?![A-Za-z0-9_])' % name, lit.group()):
                    out.append((number, name))
    return out


def sources(root):
    for top in SCAN:
        for dirpath, dirnames, filenames in os.walk(os.path.join(root, top)):
            dirnames.sort()
            for f in sorted(filenames):
                if f.endswith(EXTENSIONS):
                    full = os.path.join(dirpath, f)
                    rel = os.path.relpath(full, root).replace(os.sep, '/')
                    if not any(re.search(p, rel) for p, _ in SKIP):
                        yield rel, full


def check_meson(root):
    errors = []
    options = open(os.path.join(root, MESON_OPTIONS), encoding='utf-8').read()
    if not re.search(r"option\(\s*'%s'\s*,\s*type\s*:\s*'boolean'\s*,\s*value\s*:\s*false\b" % OPTION, options):
        errors.append('%s: no boolean option %s with value false' % (MESON_OPTIONS, OPTION))
    build = open(os.path.join(root, 'meson.build'), encoding='utf-8').read()
    if not re.search(r"if\s+get_option\('%s'\)\s*\n(?:[^\n]*\n){0,3}?[^\n]*'-D%s=1'" % (OPTION, MACRO), build):
        errors.append("meson.build: get_option('%s') does not add -D%s=1" % (OPTION, MACRO))
    return errors


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument('--root', default=DEFAULT_ROOT)
    a = p.parse_args(argv)
    errors = check_meson(a.root)
    seen_lab, files = set(), 0
    for rel, full in sources(a.root):
        text = open(full, encoding='utf-8', errors='replace').read()
        if not any(name in text for name in FORBIDDEN):
            continue
        files += 1
        try:
            for line, name in findings(rel, text, lab=False):
                errors.append('%s:%d: the release flavour still compiles %s' % (rel, line, name))
            seen_lab.update(name for _, name in findings(rel, text, lab=True))
        except Unsupported as e:
            errors.append('%s: a conditional this test cannot read: %s' % (rel, e))
    for name in FORBIDDEN:
        if name not in seen_lab:
            errors.append('%s: not found in the lab flavour either: the scan misses its lookup, or the name is '
                          'gone and belongs out of this list' % name)
    for e in errors:
        print('FAIL: ' + e)
    print('amdgpu-wddm release flavour: %s, %d forbidden name(s), %d file(s) that mention one'
          % ('FAIL' if errors else 'PASS', len(FORBIDDEN), files))
    return 1 if errors else 0


if __name__ == '__main__':
    sys.exit(main())
