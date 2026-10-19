#!/usr/bin/env python3
"""Validate image format capability requirements using actual backend output."""
import pathlib
import struct
import subprocess
import sys

exe, validator, output = sys.argv[1:]
subprocess.run([exe, output], check=True)
paths = list(pathlib.Path(output).glob('*.spv'))
assert len(paths) == 17, len(paths)
for path in paths:
    subprocess.run([validator, '--target-env', 'vulkan1.1', str(path)], check=True,
                   stdout=subprocess.PIPE)
    words = struct.unpack('<' + 'I' * (path.stat().st_size // 4), path.read_bytes())
    capabilities = set()
    i = 5
    while i < len(words):
        size, opcode = words[i] >> 16, words[i] & 65535
        assert size
        if opcode == 17:
            capabilities.add(words[i + 1])
        i += size
    if path.stem == 'typeless':
        assert 56 in capabilities, path
    else:
        buffer, format = map(int, path.stem.split('_'))
        assert (49 in capabilities) == (format in (1, 2, 3, 4, 6)), (path, capabilities)
print('Validated all 16 storage image/texel buffer format modules and typeless-write rejection/support.')
