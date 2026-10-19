#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Validate the production backend's descriptor-indexed executable kernels."""
import pathlib
import struct
import subprocess
import sys

generator, validator, directory = sys.argv[1:]
root = pathlib.Path(directory)
root.mkdir(parents=True, exist_ok=True)
subprocess.run([generator, str(root)], check=True)
modules = sorted(root.glob('*.spv'))
assert len(modules) == 18, len(modules)
for path in modules:
    env = 'vulkan1.1spv1.4' if path.stem.endswith('_66560') else 'vulkan1.1'
    validation = subprocess.run([validator, '--target-env', env, str(path)], capture_output=True, text=True)
    assert validation.returncode == 0, f'{path.name}: {validation.stderr}'
    words = struct.unpack('<' + 'I' * (path.stat().st_size // 4), path.read_bytes())
    operations, decorations, sets, bindings = [], [], {}, {}
    current_function, function_ops, switches, loads = None, {}, [], []
    i = 5
    while i < len(words):
        size, opcode = words[i] >> 16, words[i] & 65535
        assert size
        a = words[i+1:i+size]
        operations.append((opcode, a))
        if opcode == 54:
            current_function = a[1]
            function_ops[current_function] = []
        if current_function is not None: function_ops[current_function].append((opcode, a))
        if opcode == 56: current_function = None
        if opcode == 71:
            decorations.append(a)
            if a[1] == 34: sets[a[0]] = a[2]
            if a[1] == 33: bindings[a[0]] = a[2]
        if opcode == 251: switches.append(a)
        i += size
    dynamic_bindings = {bindings[key] for key, descriptor_set in sets.items() if descriptor_set == 1}
    assert dynamic_bindings == {0, 1, 2, 3, 4}, (path, dynamic_bindings)
    assert sum(op == 29 for op, a in operations) >= 5, (path, 'missing runtime descriptor arrays')
    nonuniform = {a[0] for a in decorations if a[1] == 5300}
    assert nonuniform, (path, 'missing NonUniform decoration')
    resources = {key for key, descriptor_set in sets.items()
                 if descriptor_set == 1 and bindings[key] != 1}
    pointers = {a[1] for op, a in operations if op == 65 and a[2] in resources}
    assert pointers <= nonuniform, (path, 'resource pointers must be NonUniform')
    for op, a in operations:
        if op == 61 and a[2] in pointers:
            assert a[1] in nonuniform, (path, 'loaded image/sampler must be NonUniform')
        if op in (88, 95, 96):
            assert a[2] in nonuniform, (path, 'final image operand must be NonUniform')
        if op == 86:
            assert {a[1], a[2], a[3]} <= nonuniform, (path, 'sampled image and components must be NonUniform')
    assert len(switches) == 1, (path, 'typed access must be selected by control flow')
    helper = next(ops for ops in function_ops.values() if any(op == 251 for op, a in ops))
    assert sum(op == 230 for op, a in helper) >= 3, (path, 'missing atomic first-fault guards')
    # Both fault guards and typed dispatch live in a helper, outside the IR CFG.
    assert len(function_ops) == 2, (path, 'unexpected function count')
    image_ops = [op for op, a in helper if op in (88, 95, 96, 103, 104, 106)]
    if path.stem.startswith(('sample', 'transform', 'phi')):
        assert image_ops.count(88) == 3, (path, image_ops)
    if path.stem.startswith('fetch'): assert image_ops.count(95) == 3, (path, image_ops)
    if path.stem.startswith('offset'): assert image_ops.count(96) == 3, (path, image_ops)
    if path.stem.startswith(('fetch', 'query')):
        sampler_type_ids = {a[0] for op, a in operations if op == 26}
        assert not any(op == 61 and a[0] in sampler_type_ids for op, a in helper), (path, 'samplerless operation loaded a sampler')
print(f'Validated {len(modules)} executable dynamic texture modules, typed dispatch, samplerless access and atomic fault guards.')
