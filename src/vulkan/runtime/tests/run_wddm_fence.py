"""Run production ownership code against inert graphics boundaries.

--baseline extracts source with git show without modifying the old revision.
Only the separate thread fixture calls Windows (CPU threads, no graphics).
"""
import argparse
import hashlib
import json
import os
import pathlib
import re
import subprocess


def function(source, name):
    match = re.search(
        r'(?m)^(?:static\s+)?(?:VkResult|void)\s*\n' + re.escape(name) + r'\s*\(',
        source,
    )
    if not match:
        raise RuntimeError('Missing production function: ' + name)
    start = source.index('{', match.end())
    depth = 0
    tokens = re.compile(r'/\*.*?\*/|//[^\n]*|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|[{}]', re.S)
    for token in tokens.finditer(source, start):
        if token.group() == '{':
            depth += 1
        elif token.group() == '}':
            depth -= 1
            if depth == 0:
                return match.start(), token.end(), source[match.start():token.end()]
    raise RuntimeError('Unclosed function: ' + name)


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
        'vk_wddm2_monitored_fence.c', 'vk_wddm2_monitored_fence.h', 'vk_sync.c')}
    source, header, sync = (data.decode().replace('\r\n', '\n') for data in inputs.values())
    # Retain every ownership helper and the production dispatch table. Unrelated
    # signal/value/wait functions are stubbed to exclude driver dependencies.
    source = source[:source.index('\nVkResult\nvk_wddm2_check_device_status')]
    for name in ('vk_wddm2_monitored_fence_signal',
                 'vk_wddm2_monitored_fence_get_value',
                 'vk_wddm2_monitored_fence_wait_many'):
        begin, end, _ = function(source, name)
        source = source[:begin] + source[end:]
    source = re.sub(r'^#include[^\n]*\n', '', source, flags=re.M)
    structure = re.search(r'struct vk_wddm2_monitored_fence \{.*?\n\};', header, re.S).group()
    wrappers = '\n'.join(function(sync, name)[2] for name in (
        'vk_sync_import_win32_handle', 'vk_sync_export_win32_handle',
        'vk_sync_set_win32_export_params'))
    unit = '\n'.join([
        '#define _WIN32 1', '#define NDEBUG 1', '#include "wddm_fence_mock.h"',
        structure, 'extern const struct vk_sync_type vk_wddm2_monitored_fence_type;',
        'static bool vk_sync_type_is_wddm2_monitored_fence(const struct vk_sync_type *t)',
        '{ return t == &vk_wddm2_monitored_fence_type; }', source, wrappers,
        '#include "wddm_fence_test.c"', '',
    ])
    generated = out / 'production-mock.c'
    generated.write_text(unit, encoding='utf-8')
    env = os.environ.copy()
    env['TMP'] = env['TEMP'] = str(out)
    exe = out / 'wddm-fence-test.exe'
    command = [args.compiler, '-std=c11', '-Wall', '-Wextra', '-Werror',
               '-Wno-missing-field-initializers', '-Wno-unused-parameter', '-Wno-unused-function', '-Wno-unused-variable',
               '-DUSE_GCC_ATOMIC_BUILTINS', '-I' + str(repo / 'src'), '-I' + str(repo / 'include'), '-I' + str(here), str(generated), str(here / 'wddm_fence_threads.c'),
               '-o', str(exe)]
    build = subprocess.run(command, capture_output=True, text=True, env=env, timeout=60)
    (out / 'build.txt').write_text(build.stdout + build.stderr, encoding='utf-8')
    if build.returncode:
        print(build.stderr)
        return build.returncode
    run = subprocess.run([str(exe)], capture_output=True, text=True, env=env, timeout=10)
    (out / 'result.txt').write_text(run.stdout + run.stderr, encoding='utf-8')
    manifest = {
        'revision': args.baseline or 'working',
        'production_sha256': {name: sha(data) for name, data in inputs.items()},
        'fixture_sha256': {name: sha((here / name).read_bytes()) for name in (
            'wddm_fence_mock.h', 'wddm_fence_test.c', 'wddm_fence_threads.c',
            'run_wddm_fence.py')},
        'utility_sha256': {name: sha((repo / name).read_bytes()) for name in ('src/util/u_atomic.h', 'include/no_extern_c.h')},
        'generated_sha256': sha(generated.read_bytes()),
        'artifact_sha256': sha(exe.read_bytes()),
        'compiler_sha256': sha(pathlib.Path(args.compiler).read_bytes()),
        'exit': run.returncode, 'command': command,
    }
    (out / 'manifest.json').write_text(json.dumps(manifest, indent=2), encoding='utf-8')
    print(run.stdout, end='')
    return run.returncode


if __name__ == '__main__':
    raise SystemExit(main())
