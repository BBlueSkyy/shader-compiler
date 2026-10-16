#!/usr/bin/env python3
"""Check primitive output lifetime in production-emitted straight-line fixtures.

Vulkan makes output variables undefined after EmitVertex. Track stores/loads of
Layer and ViewportIndex along the fixture's only path, explicitly clearing their
initialized state at every emit. This catches the first-vertex-only store bug.
"""
import pathlib
import re
import subprocess
import sys
module = pathlib.Path(sys.argv[1]) / 'primitive-lowered.spv'
text = subprocess.check_output(['spirv-dis', str(module)], text=True)
primitive = set(re.findall(r'OpDecorate (%\w+) BuiltIn (?:Layer|ViewportIndex)', text))
assert len(primitive) == 2, 'fixture must write both primitive builtins'
initialized = set()
vertices = 0
for line in text.splitlines():
    load = re.search(r'OpLoad %\w+ (%\w+)', line)
    if load and load[1] in primitive:
        assert load[1] in initialized, 'read of undefined primitive output after EmitVertex'
    store = re.search(r'OpStore (%\w+) ', line)
    if store and store[1] in primitive:
        initialized.add(store[1])
    if 'OpEmitVertex' in line:
        assert primitive <= initialized, 'primitive output undefined at emitted vertex'
        vertices += 1
        initialized.clear()
assert vertices == 3, 'expected three output vertices'
print('PASS primitive outputs are defined at every emitted vertex')
