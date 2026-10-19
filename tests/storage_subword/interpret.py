"""Small interpreter for the integer/memory subset emitted by these fixtures.

The production SPIR-V is also checked by spirv-val. This executes its actual
instructions to check byte addressing, truncation, adjacent-bit preservation,
and a deterministic concurrent update that forces every CAS to retry once.
"""


def execute(ops, bits, native, inject, writable=True):
    constants, types, bindings, functions = {}, {}, {}, {}
    current = None
    for op, a in ops:
        if op == 15:  # OpEntryPoint
            entry = a[1]
        elif op == 21:  # OpTypeInt
            types[a[0]] = a[1]
        elif op == 32:  # OpTypePointer
            types[a[0]] = types.get(a[2], 32)
        elif op == 43:  # OpConstant
            constants[a[1]] = a[2]
        elif op == 71 and a[1] == 33:  # Binding decoration
            bindings[a[0]] = a[2]
        elif op == 54:  # OpFunction
            current = ([], [])
            functions[a[1]] = current
        elif op == 56:
            current = None
        elif current is not None:
            if op == 55:
                current[0].append(a[1])
            else:
                current[1].append((op, a))
    initial = 0x76543210
    memory = {(binding, word): initial for binding in range(2) for word in range(2)}
    expected = memory.copy()
    writes = []
    retries = 0

    def reference(binding, word, shift, count, value):
        mask = ((1 << count) - 1) << shift
        expected[binding, word] = (expected[binding, word] & ~mask) | ((value << shift) & mask)
        writes.append((binding, word * 4 + shift // 8, count, value & ((1 << count) - 1)))

    def run(function, args=()):
        nonlocal retries
        parameters, instructions = functions[function]
        values = constants | dict(zip(parameters, args))
        labels = {a[0]: i for i, (op, a) in enumerate(instructions) if op == 248}
        metadata = {}
        injected = False
        pc = 0
        steps = 0
        while pc < len(instructions):
            steps += 1
            assert steps < 10000, 'CAS loop failed to converge'
            op, a = instructions[pc]
            pc += 1
            if op in (248, 246):  # Label, LoopMerge
                continue
            if op == 253:
                return
            if op == 249:
                pc = labels[a[0]]
                continue
            if op == 250:
                pc = labels[a[1] if values[a[0]] else a[2]]
                continue
            if op == 57:  # FunctionCall
                run(a[2], [values[x] for x in a[3:]])
                continue
            if op == 65:  # AccessChain
                values[a[1]] = (bindings[a[2]], values[a[-1]] * (types[a[0]] // 8), types[a[0]])
                continue
            if op in (61, 227):  # Load, AtomicLoad
                binding, address, width = values[a[2]]
                values[a[1]] = (memory[binding, address // 4] >> ((address % 4) * 8)) & ((1 << width) - 1)
                continue
            if op == 62:  # Store
                binding, address, width = values[a[0]]
                shift = (address % 4) * 8
                mask = ((1 << width) - 1) << shift
                value = values[a[1]]
                memory[binding, address // 4] = (memory[binding, address // 4] & ~mask) | ((value << shift) & mask)
                reference(binding, address // 4, shift, width, value)
                continue
            if op == 230:  # AtomicCompareExchange
                binding, address, width = values[a[2]]
                assert width == 32
                shift, count, value = metadata[a[6]]
                if inject and not injected:
                    # Flip a bit in a neighboring lane after the atomic load.
                    adjacent = 1 << (0 if shift else count)
                    memory[binding, address // 4] ^= adjacent
                    expected[binding, address // 4] ^= adjacent
                    injected = True
                old = memory[binding, address // 4]
                values[a[1]] = old
                if old == values[a[7]]:
                    memory[binding, address // 4] = values[a[6]]
                    reference(binding, address // 4, shift, count, value)
                else:
                    retries += 1
                continue
            lhs = values.get(a[2], 0) if len(a) > 2 else 0
            rhs = values.get(a[3], 0) if len(a) > 3 else 0
            if op in (113, 114, 124):  # UConvert, SConvert, Bitcast
                result = lhs & ((1 << types[a[0]]) - 1)
            elif op == 128:
                result = lhs + rhs
            elif op == 194:
                result = lhs >> rhs
            elif op == 196:
                result = lhs << rhs
            elif op == 199:
                result = lhs & rhs
            elif op == 170:
                result = lhs == rhs
            elif op == 201:  # BitFieldInsert
                shift, count = values[a[4]], values[a[5]]
                mask = ((1 << count) - 1) << shift
                result = (lhs & ~mask) | ((rhs << shift) & mask)
                metadata[a[1]] = (shift, count, rhs)
            elif op in (202, 203):  # BitFieldSExtract, BitFieldUExtract
                shift, count = rhs, values[a[4]]
                result = (lhs >> shift) & ((1 << count) - 1)
            else:
                raise AssertionError(f'Unexpected opcode {op}')
            values[a[1]] = int(result) & 0xffffffff
    run(entry)
    assert memory == expected, (memory, expected)
    # Independent expected byte addresses model Maxwell alignment: halfword low
    # address bit is ignored, while byte accesses retain both low address bits.
    widths = [8, 8, 16, 16] if bits is None else [bits]
    expected_writes = [(binding, offset if width == 8 else offset & ~1, width,
                        0xfedcba98 & ((1 << width) - 1))
                       for binding in range(2) for offset in range(8) for _ in range(2)
                       for width in widths] if writable else []
    assert writes == expected_writes, (writes, expected_writes)
    assert retries == (len(writes) if inject and not native else 0), retries
