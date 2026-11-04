"""Run the production CPU wait of the WDDM monitored fence against an inert
kernel boundary (BD-200, VK_SYNC_WAIT_PENDING).

The generated unit holds the production fence structure, the production
functions of the CPU wait and wddm_fence_pending_test.c. --baseline takes the
fence source from a git revision with git show, without a checkout: a
revision before BD-200 has no pending wait, so the runner supplies a note
function that does nothing, and the pending cases fail (negative control).
"""
import argparse
import hashlib
import json
import os
import pathlib
import re
import subprocess

FUNCTIONS = (
    'to_wddm2_monitored_fence',
    'NTSTATUS_to_VkResult',
)
# The BD-179 helpers. A line without the BD-179 port (desktop) has no fence
# poll; before BD-200 it also has no liveness helper and no slice variable.
OPTIONAL_FUNCTIONS = (
    'vk_wddm2_monitored_fence_poll',
    'vk_wddm2_wait_slice_device_lost',
)
SLICE_STUB = ('/* Not in this revision: the cases still set it. */\n'
              'uint64_t vk_wddm2_monitored_fence_wait_slice_ns = 1000000000ull;')
PENDING_BEGIN = 'static once_flag vk_wddm2_pending_once'
PENDING_LAST = 'vk_wddm2_monitored_fence_wait_pending'
BASELINE_NOTE = '''void
vk_wddm2_monitored_fence_note_submitted(struct vk_wddm2_monitored_fence *fence, uint64_t value)
{
   (void)fence;
   (void)value;
}
'''


def definition(source, name):
    """The definition of a top-level function: its return type line, the
    name at the start of the next line and the body. Declarations without a
    body are skipped."""
    pattern = re.compile(r'(?m)^[A-Za-z_][^\n;{}()]*\n' + re.escape(name) + r'\s*\(')
    tokens = re.compile(r'/\*.*?\*/|//[^\n]*|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|[{};]', re.S)
    for match in pattern.finditer(source):
        first = tokens.search(source, match.end())
        while first and first.group() not in '{;':
            first = tokens.search(source, first.end())
        if not first or first.group() == ';':
            continue
        depth = 0
        for token in tokens.finditer(source, first.start()):
            if token.group() == '{':
                depth += 1
            elif token.group() == '}':
                depth -= 1
                if depth == 0:
                    return match.start(), token.end()
        raise RuntimeError('Unclosed function: ' + name)
    raise RuntimeError('Missing production function: ' + name)


def text(source, name):
    begin, end = definition(source, name)
    return source[begin:end] + '\n'


def sha(data):
    return hashlib.sha256(data).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out', required=True)
    parser.add_argument('--compiler', required=True)
    parser.add_argument('--baseline')
    args = parser.parse_args()
    here = pathlib.Path(__file__).resolve().parent
    repo = here.parents[3]
    out = pathlib.Path(args.out).resolve()
    out.mkdir(parents=True, exist_ok=True)

    def read(name):
        relative = 'src/vulkan/runtime/' + name
        if args.baseline:
            return subprocess.check_output(
                ['git', '-C', str(repo), 'show', args.baseline + ':' + relative],
                timeout=30,
            )
        return (repo / relative).read_bytes()

    inputs = {name: read(name) for name in (
        'vk_wddm2_monitored_fence.c', 'vk_wddm2_monitored_fence.h')}
    source, header = (data.decode().replace('\r\n', '\n') for data in inputs.values())

    structure = re.search(r'struct vk_wddm2_monitored_fence \{.*?\n\};', header, re.S).group()
    slice_ns = re.search(r'(?m)^uint64_t vk_wddm2_monitored_fence_wait_slice_ns = [^;]*;', source)
    parts = [text(source, name) for name in FUNCTIONS]
    for name in OPTIONAL_FUNCTIONS:
        try:
            parts.append(text(source, name))
        except RuntimeError:
            pass
    parts.append((slice_ns.group() if slice_ns else SLICE_STUB) + '\n')
    if PENDING_BEGIN in source:
        begin = source.index(PENDING_BEGIN)
        _, end = definition(source, PENDING_LAST)
        if end < begin:
            raise RuntimeError('Pending block out of order')
        parts.append(source[begin:end] + '\n')
        mode = 'pending'
    else:
        parts.append(BASELINE_NOTE)
        mode = 'baseline-without-pending'
    # A line whose type has .move (the ASSISTED timeline mode needs it): the
    # move must carry the submitted value with the payload.
    if re.search(r'(?m)^vk_wddm2_monitored_fence_move\s*\(', source):
        parts.append('#define HAVE_FENCE_MOVE 1\n')
        parts.append(text(source, 'vk_wddm2_monitored_fence_move'))
    parts.append(text(source, 'vk_wddm2_monitored_fence_signal'))
    parts.append(text(source, 'vk_wddm2_monitored_fence_wait_many'))

    flags = []
    if 'shared_handle_imported' in structure:
        flags.append('#define FENCE_HAS_IMPORTED_FLAG 1')
    unit = '\n'.join([
        *flags,
        '#define WDDM_PENDING_STAGE 1', '#include "wddm_fence_pending_mock.h"',
        '#undef WDDM_PENDING_STAGE', structure,
        '#define WDDM_PENDING_STAGE 2', '#include "wddm_fence_pending_mock.h"',
        *parts,
        '#include "wddm_fence_pending_test.c"', '',
    ])
    generated = out / 'production-pending.c'
    generated.write_text(unit, encoding='utf-8')
    env = os.environ.copy()
    env['TMP'] = env['TEMP'] = str(out)
    exe = out / 'wddm-fence-pending-test.exe'
    includes = ['-I' + str(repo / 'src'), '-I' + str(repo / 'include'),
                '-I' + str(repo / 'src/vulkan/runtime'), '-I' + str(here)]
    c11 = [str(repo / 'src/c11/impl/threads_win32.c'), str(repo / 'src/c11/impl/time.c')]
    command = [args.compiler, '-std=c11', '-O1', '-Wall', '-Wextra', '-Werror',
               '-Wno-missing-field-initializers', '-Wno-unused-parameter', '-Wno-unused-function',
               # thrd_exit in src/c11/impl/threads_win32.c, not code under test.
               '-Wno-invalid-noreturn',
               # The desktop completion wait compares os_time_get_nano() with
               # a uint64_t deadline; the Mesa build does not make it an error.
               '-Wno-sign-compare',
               '-DUSE_GCC_ATOMIC_BUILTINS', '-DHAVE_STRUCT_TIMESPEC', *includes, str(generated), *c11, '-o', str(exe)]
    build = subprocess.run(command, capture_output=True, text=True, env=env, timeout=120)
    (out / 'build.txt').write_text(build.stdout + build.stderr, encoding='utf-8')
    if build.returncode:
        print(build.stdout + build.stderr)
        return build.returncode
    try:
        run = subprocess.run([str(exe)], capture_output=True, text=True, env=env, timeout=60)
        output, code = run.stdout + run.stderr, run.returncode
    except subprocess.TimeoutExpired as error:
        partial = error.stdout or ''
        if isinstance(partial, bytes):
            partial = partial.decode(errors='replace')
        output = partial + '\nTIMEOUT after 60 s\n'
        code = 124
    (out / 'result.txt').write_text(output, encoding='utf-8')
    manifest = {
        'revision': args.baseline or 'working',
        'mode': mode,
        'production_sha256': {name: sha(data) for name, data in inputs.items()},
        'fixture_sha256': {name: sha((here / name).read_bytes()) for name in (
            'wddm_fence_pending_mock.h', 'wddm_fence_pending_test.c',
            'run_wddm_fence_pending.py')},
        'generated_sha256': sha(generated.read_bytes()),
        'artifact_sha256': sha(exe.read_bytes()),
        'compiler_sha256': sha(pathlib.Path(args.compiler).read_bytes()),
        'exit': code, 'command': command,
    }
    (out / 'manifest.json').write_text(json.dumps(manifest, indent=2), encoding='utf-8')
    print(output, end='')
    return code


if __name__ == '__main__':
    raise SystemExit(main())
