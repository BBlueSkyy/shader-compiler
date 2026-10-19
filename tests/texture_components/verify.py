#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Check actual image scalar/result types and bit-preserving return to float IR."""
import pathlib
import struct
import subprocess
import sys

generator, validator, output = sys.argv[1:]
root = pathlib.Path(output)
root.mkdir(parents=True, exist_ok=True)
subprocess.run([generator, str(root)], check=True)
paths = sorted(root.glob('*.spv'))
assert len(paths) == 36, len(paths)
for path in paths:
    version, operation, sparse = map(int, path.stem.split('_'))
    env = 'vulkan1.1spv1.4' if version >= 0x10400 else 'vulkan1.1'
    result = subprocess.run([validator, '--target-env', env, str(path)], capture_output=True, text=True)
    assert result.returncode == 0, f'{path.name}: {result.stderr}'
    words = struct.unpack('<' + 'I' * (path.stat().st_size // 4), path.read_bytes())
    types, definitions, casts, extracts, images = {}, {}, [], [], []
    i = 5
    while i < len(words):
        size, opcode = words[i] >> 16, words[i] & 65535
        a = words[i + 1:i + size]
        assert size
        if opcode in (21, 22, 23, 25, 27, 30):
            types[a[0]] = (opcode, a[1:])
        if opcode in (61, 80, 81, 87, 88, 95, 96, 100, 124, 305, 306, 313, 314):
            definitions[a[1]] = (a[0], opcode, a[2:])
        if opcode == 124:
            casts.append(a)
        if opcode == 81:
            extracts.append(a)
        if opcode in (87, 88, 95, 96, 305, 306, 313, 314):
            images.append((opcode, a))
        assert opcode not in (109, 110, 111, 112), (path, 'numeric conversion loses texture bits')
        i += size
    assert len(images) == 3, (path, len(images))
    expected_opcode = [95, 95, 87, 88, 96, 88][operation] + (218 if sparse else 0)
    for component, (opcode, a) in enumerate(images):
        assert opcode == expected_opcode, (path, opcode, expected_opcode)
        image_type = definitions[a[2]][0]
        if types[image_type][0] == 27:
            image_type = types[image_type][1][0]
        assert types[image_type][0] == 25, path
        scalar_type = types[image_type][1][0]
        scalar = types[scalar_type]
        expected_scalar = (22, (32,)) if component == 0 else (21, (32, component == 2))
        assert scalar == expected_scalar, (path, component, scalar, expected_scalar)
        result_type = types[a[0]][1][1] if sparse else a[0]
        assert types[result_type] == (23, (scalar_type, 4)), (path, component, 'image result scalar type')
        if sparse:
            payloads = [e for e in extracts if e[2:] == (a[1], 1)]
            assert len(payloads) == 1, (path, 'sparse texel payload')
            result_id = payloads[0][1]
        else:
            result_id = a[1]
        bitcasts = [c for c in casts if c[2] == result_id]
        assert len(bitcasts) == (component != 0), (path, component, 'bit-preserving conversion')
        if component:
            cast = bitcasts[0]
            vector = types[cast[0]]
            assert vector[0] == 23 and vector[1][1] == 4 and types[vector[1][0]][0] == 22, (path, 'float IR result')
print('Validated 36 mixed Float/Uint/Sint modules with 108 buffer fetch, texture fetch, sampling, gather, and gradient operations, including sparse results and bitcasts.')
