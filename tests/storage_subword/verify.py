#!/usr/bin/env python3
"""Validate actual backend modules and interpret subword write instructions, including CAS retries."""
import pathlib
import struct
import subprocess
import sys
from interpret import execute

exe, validator, output = sys.argv[1:]
subprocess.run([exe, output], check=True)
paths = sorted(path for path in pathlib.Path(output).glob('*.spv')
               if len(path.stem.split('_')) == 3)
assert len(paths) == 576, f'Expected 576 modules, found {len(paths)}'
for path in paths:
    version, features, kind = map(int, path.stem.split('_'))
    target = 'vulkan1.1spv1.4' if version == 0x10400 else 'vulkan1.1'
    subprocess.run([validator, '--target-env', target, str(path)], check=True,
                   stdout=subprocess.PIPE)
    mixed = kind >= 4
    writable = kind != 5
    bits = None if mixed else 8 if kind < 2 else 16
    native8 = bool(features & 1 and features & 2 and features & 8)
    native16 = bool(features & 1 and features & 4 and features & 16)
    fallback = ((mixed or bits == 8) and not native8) or ((mixed or bits == 16) and not native16)
    if writable and fallback:
        native8 = native16 = False
    native = not fallback
    words = struct.unpack('<' + 'I' * (path.stat().st_size // 4), path.read_bytes())
    ops = []
    i = 5
    while i < len(words):
        size, opcode = words[i] >> 16, words[i] & 65535
        assert size
        ops.append((opcode, words[i+1:i+size]))
        i += size
    opcodes = [op for op, _ in ops]
    assert (230 in opcodes) == (writable and fallback), (path, 'CAS selection')
    capabilities = {args[0] for op, args in ops if op == 17}
    assert (4448 in capabilities) == (native8 and (mixed or bits == 8)), (path, capabilities)
    assert (4433 in capabilities) == (native16 and (mixed or bits == 16)), (path, capabilities)
    assert 4441 not in capabilities and 4442 not in capabilities, (path, 'variable pointers')
    if not (features & 2):
        assert not any(op == 21 and args[1] == 8 for op, args in ops), path
    if not (features & 4):
        assert not any(op == 21 and args[1] == 16 for op, args in ops), path
    if writable and fallback:
        assert 61 not in opcodes and 62 not in opcodes, (path, 'mixed atomic/non-atomic subword access')
        assert 227 in opcodes, (path, 'missing atomic word reads')
    elif not writable:
        assert 227 not in opcodes and 230 not in opcodes, (path, 'readonly module requires atomics')
        assert 61 in opcodes, (path, 'missing readonly load')
    execute(ops, bits, native, False, writable)
    execute(ops, bits, native, True, writable)
print('Validated and interpreted 576 generated SPIR-V modules; all fallback stores survive forced CAS retries.')
