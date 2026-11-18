#!/usr/bin/env python3
"""Build and validate the production small-storage backend; no Android SDK needed."""
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
parser.add_argument('--sanitizers', default='address,undefined')
parser.add_argument('--baseline-ref', help='isolate the old memory emitter and require it to fail spirv-val')
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
         '-Wno-changes-meaning',
         '-fsanitize=' + args.sanitizers, '-fno-sanitize-recover=all', *shlex.split(os.environ.get('CXXFLAGS', '')), '-UNDEBUG']
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
exe = build / 'small-storage-test'
subprocess.run([os.environ.get('CXX', 'c++'), '-fsanitize=' + args.sanitizers, *map(str, objects),
                '-Wl,--gc-sections', '-o', str(exe)], check=True)
modules = build / 'modules'
modules.mkdir(exist_ok=True)
subprocess.run([exe, modules], check=True, timeout=180)
count = 0
for module in sorted(modules.glob('*.spv')):
    version, mask, kind = map(int, module.stem.split('-'))
    target = 'vulkan1.1' if version == 0x10000 else 'vulkan1.1spv1.4'
    def tool(name): return str(args.spirv_tools / name) if args.spirv_tools else name
    subprocess.run([tool('spirv-val'), '--target-env', target, '--uniform-buffer-standard-layout', module], check=True)
    text = subprocess.check_output([tool('spirv-dis'), module], text=True)
    native8 = (mask & 69) == 69
    native16 = (mask & 74) == 74
    uniform8 = (mask & 81) == 81
    uniform16 = (mask & 98) == 98
    assert ('OpCapability StorageBuffer8BitAccess' in text) == native8, module
    assert ('OpCapability StorageBuffer16BitAccess' in text) == native16, module
    assert ('OpCapability UniformAndStorageBuffer8BitAccess' in text) == (uniform8 and kind in (4, 5)), module
    assert ('OpCapability UniformAndStorageBuffer16BitAccess' in text) == (uniform16 and kind in (4, 5)), module
    need_cas = (kind in (0,1,6,7) and not native8) or (kind in (0,2,6,7) and not native16)
    assert ('OpAtomicCompareExchange' in text) == need_cas, module
    assert 'VariablePointers' not in text, module
    if need_cas:
        assert 'OpAtomicLoad' in text, module
    if not mask & 1: assert 'OpCapability Int8' not in text, module
    if not mask & 2: assert 'OpCapability Int16' not in text, module
    count += 1
assert count == 2048, count
print(f'{count} modules passed spirv-val, capability/helper matrix and Vulkan 1.1/1.1+SPIR-V 1.4 validation')
if args.baseline_ref:
    old = build / 'baseline-memory.cpp'
    old.write_text(subprocess.check_output(['git', '-C', root, 'show', f'{args.baseline_ref}:backend/spirv/emit_spirv_memory.cpp'], text=True))
    old_obj = compile_source(old)
    baseline = build / 'baseline-storage-test'
    subprocess.run([os.environ.get('CXX', 'c++'), '-fsanitize=' + args.sanitizers,
        *[str(o) for o in objects if o.name != 'emit_spirv_memory.o'], str(old_obj),
        '-Wl,--gc-sections', '-o', str(baseline)], check=True)
    output = build / 'baseline-modules'
    output.mkdir(exist_ok=True)
    old_run = subprocess.run([baseline, output, '--single'], capture_output=True, text=True)
    if old_run.returncode:
        assert old_run.returncode == -6 and 'result_type.value != 0' in old_run.stderr, old_run.stderr
        print('Confirmed pre-fix writer aborts in Sirit on an undefined native pointer type')
    else:
        result = subprocess.run([tool('spirv-val'), '--target-env', 'vulkan1.1', output / '65536-67-0.spv'], capture_output=True, text=True)
        assert result.returncode != 0, 'old unconditionally native writer unexpectedly passed'
        print('Confirmed pre-fix writer fails SPIR-V validation:', result.stderr.strip().splitlines()[0])
