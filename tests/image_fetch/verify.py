#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Validate production modules and evaluate the integer coordinates fed to each fetch."""
import pathlib
import struct
import subprocess
import sys

generator, validator, output = sys.argv[1:]
root = pathlib.Path(output)
root.mkdir(parents=True, exist_ok=True)
for path in root.glob('*.spv'):
    path.unlink()
subprocess.run([generator, str(root)], check=True)
paths = sorted(root.glob('*.spv'))
assert len(paths) == 243, len(paths)
shapes = [(1, False), (1, True), (2, False), (2, True), (3, False), (1, False), (2, False)]
for path in paths:
    query = path.stem.startswith('query_')
    if query:
        _, version, operation = path.stem.split('_')
        version, operation = int(version), int(operation)
        samplerless = operation == 1
    else:
        version, shape, offset_kind, sparse, samplerless = map(int, path.stem.split('_'))
    env = 'vulkan1.1spv1.4' if version >= 0x10400 else 'vulkan1.1'
    result = subprocess.run([validator, '--target-env', env, str(path)], capture_output=True, text=True)
    assert result.returncode == 0, f'{path.name}: {result.stderr}'
    words = struct.unpack('<' + 'I' * (path.stat().st_size // 4), path.read_bytes())
    values = {}
    fetches = []
    opcodes = []
    i = 5
    while i < len(words):
        size, opcode = words[i] >> 16, words[i] & 65535
        opcodes.append(opcode)
        assert size
        a = words[i + 1:i + size]
        if opcode == 43:  # OpConstant
            values[a[1]] = a[2]
        elif opcode in (44, 80) and all(v in values for v in a[2:]):  # Known composites
            parts = [values[v] for v in a[2:]]
            values[a[1]] = tuple(x for part in parts for x in (part if isinstance(part, tuple) else (part,)))
        elif opcode == 81 and a[2] in values:  # OpCompositeExtract
            values[a[1]] = values[a[2]][a[3]]
        elif opcode == 128:  # OpIAdd
            left, right = values[a[2]], values[a[3]]
            values[a[1]] = tuple((x + y) & 0xffffffff for x, y in zip(left, right)) if isinstance(left, tuple) else (left + right) & 0xffffffff
        elif opcode in (95, 313):  # OpImageFetch / OpImageSparseFetch
            fetches.append((opcode, a))
        i += size
    # OpTypeSampledImage / OpImage must occur only when a sampler is retained.
    needs_sampler = not samplerless and (query or shape != 5)
    assert (27 in opcodes) == needs_sampler, (path, 'sampled image type')
    assert (100 in opcodes) == (needs_sampler and (not query or operation < 2)), (path, 'OpImage')
    if query:
        assert (103 in opcodes) == (operation < 2), (path, 'OpImageQuerySizeLod')
        assert (105 in opcodes) == (operation == 2), (path, 'OpImageQueryLod')
        continue
    assert len(fetches) == 1, path
    opcode, a = fetches[0]
    assert opcode == (313 if sparse else 95), path
    spatial, array = shapes[shape]
    expected = [3, 11, 7][:spatial]
    if offset_kind:
        expected = [x + y for x, y in zip(expected, [-2, 3, -1])]
    if array:
        expected.append(23)
    expected = expected[0] if len(expected) == 1 else tuple(expected)
    assert values[a[3]] == expected, (path, values[a[3]], expected)
    mask = a[4] if len(a) > 4 else 0
    assert mask == (0 if shape == 5 else 2), (path, mask)
    if shape != 5:
        assert values[a[5]] == 2, (path, 'LOD lost')
print('Validated 234 texture fetch and 9 query modules, including signed offsets, array layers, LOD, sparse fetches, and samplerless descriptors.')
