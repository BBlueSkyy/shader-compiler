#!/usr/bin/env python3
"""Build actual emitters and the test-only CPU oracle; no Android SDK needed."""
import argparse
import os
import shlex
import json
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
import subprocess

parser = argparse.ArgumentParser()
parser.add_argument('--deps', type=Path, required=True)
parser.add_argument('--build-dir', type=Path, required=True)
parser.add_argument('--boost-include', type=Path)
parser.add_argument('--spirv-tools', type=Path, help='directory containing spirv-val and spirv-dis; defaults to PATH')
parser.add_argument('--jobs', type=int, default=2)
parser.add_argument('--baseline-ref', required=True, help='compiler commit before this change, used for byte-identical supported-stage checks')
args = parser.parse_args()
root = Path(__file__).resolve().parents[2]
deps = args.deps.resolve()
build = args.build_dir.resolve()
build.mkdir(parents=True, exist_ok=True)
includes = [root / 'include', deps / 'fmt/include', deps / 'range/include',
            deps / 'sirit/include', deps / 'sirit/src', deps / 'sirit/externals/SPIRV-Headers/include']
includes += [args.boost_include] if args.boost_include else list((deps / 'boost/libs').glob('*/include'))
sources = [Path(__file__).with_name('test.cpp')]
sources += list((root / 'backend/spirv').glob('*.cpp'))
sources += list((root / 'frontend/ir').glob('*.cpp'))
sources += list((deps / 'sirit/src').rglob('*.cpp'))
flags = ['-std=c++20', '-O1', '-g', '-include', 'cstddef', '-DFMT_HEADER_ONLY',
         '-fno-strict-aliasing', '-ffunction-sections', '-fdata-sections',
         '-fsanitize=undefined', '-fno-sanitize-recover=all', *shlex.split(os.environ.get('CXXFLAGS', '')), '-UNDEBUG']
def compile_source(source):
    obj = build / (('sirit_' if source.is_relative_to(deps / 'sirit') else '') + source.stem + '.o')
    dep = obj.with_suffix('.d')
    stamp = obj.with_suffix('.json')
    command = [os.environ.get('CXX', 'c++'), *flags, *[f'-I{p}' for p in includes],
               '-MMD', '-MF', str(dep), '-c', str(source), '-o', str(obj)]
    inputs = shlex.split(dep.read_text().replace('\\\n', ' ').split(':', 1)[1]) if dep.exists() else []
    fresh = (obj.exists() and stamp.exists() and inputs and json.loads(stamp.read_text()) == command
             and all(Path(p).exists() and Path(p).stat().st_mtime <= obj.stat().st_mtime for p in inputs))
    if not fresh:
        subprocess.run(command, check=True)
        stamp.write_text(json.dumps(command))
    return obj

with ThreadPoolExecutor(max_workers=args.jobs) as executor:
    objects = list(executor.map(compile_source, sources))
exe = build / 'warp32-test'
subprocess.run([os.environ.get('CXX', 'c++'), '-fsanitize=undefined', *map(str, objects),
                '-Wl,--gc-sections', '-o', str(exe)], check=True)
modules = build / 'modules'
modules.mkdir(exist_ok=True)
for old in modules.glob('*.spv'):
    old.unlink()
subprocess.run([exe, modules], check=True, timeout=60)
# Reuse the full compiler, substituting only the baseline warp emitter.
baseline_source = build / 'baseline-warp.cpp'
baseline_text = subprocess.check_output(['git', '-C', str(root), 'show',
    f'{args.baseline_ref}:backend/spirv/emit_spirv_warp.cpp'], text=True)
if not baseline_source.exists() or baseline_source.read_text() != baseline_text:
    baseline_source.write_text(baseline_text)
baseline_object = compile_source(baseline_source)
baseline_exe = build / 'warp32-baseline'
subprocess.run([os.environ.get('CXX', 'c++'), '-fsanitize=undefined',
    *[str(o) for o in objects if o.name != 'emit_spirv_warp.o'], str(baseline_object),
    '-Wl,--gc-sections', '-o', str(baseline_exe)], check=True)
baseline_modules = build / 'baseline-modules'
baseline_modules.mkdir(exist_ok=True)
subprocess.run([baseline_exe, baseline_modules, '--emit-only'], check=True, timeout=60)
def tool(name):
    return str(args.spirv_tools / name) if args.spirv_tools else name
count = 0
vulkan_passed = 0
inherited = []
for module in sorted(modules.glob('*.spv')):
    subprocess.run([tool('spirv-val'), '--target-env', 'spv1.3', module], check=True)
    vk = subprocess.run([tool('spirv-val'), '--target-env', 'vulkan1.1', module], capture_output=True, text=True)
    assembly = subprocess.check_output([tool('spirv-dis'), module], text=True)
    if module.name.startswith('unsupported-'):
        assert vk.returncode == 0, (module, vk.stderr)
        assert 'GroupNonUniform' not in assembly, module
        assert 'BuiltIn Subgroup' not in assembly, module
        assert 'Subgroup' not in assembly, module
    else:
        assert module.read_bytes() == (baseline_modules / module.name).read_bytes(), module
        assert 'OpGroupNonUniform' in assembly, module
        if vk.returncode:
            expected = {'0': 'VUID-StandaloneSpirv-Flat-06202', '4': 'VUID-StandaloneSpirv-Flat-04744'}
            assert expected.get(module.name.split('-')[1], 'unexpected failure') in vk.stderr, vk.stderr
            inherited.append((module.name, vk.stderr.strip().splitlines()[0]))
    vulkan_passed += vk.returncode == 0
    count += 1
assert count == 102, count
print(f'{count} modules passed SPIR-V 1.3 validation; {vulkan_passed} passed Vulkan 1.1')
print('All 78 unsupported-stage modules passed Vulkan validation and contain no subgroup operations/builtins/capabilities')
print('All 24 supported-stage modules are byte-identical to the baseline')
for name, error in inherited:
    print(f'INHERITED Vulkan interface limitation: {name}: {error}')
