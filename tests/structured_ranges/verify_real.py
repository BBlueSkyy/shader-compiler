#!/usr/bin/env python3
from pathlib import Path
import argparse,subprocess,tempfile,re,hashlib
def tokens(text):
    return [s.strip() for s in text.splitlines() if s.strip() and not s.lstrip().startswith(';')]

def constants(lines):
    out = {}
    for line in lines:
        m = re.fullmatch(r'(%\d+) = OpConstant (%\d+) (.+)', line)
        if m:
            key = m[2], m[3]
            assert key not in out, 'duplicate constant needs a stronger comparer'
            out[key] = m[1]
    return out

def normalize_to_reference(text, reference):
    lines, ref = tokens(text), tokens(reference)
    ours, theirs = constants(lines), constants(ref)
    assert ours.keys() == theirs.keys(), 'different constants'
    mapping = {ours[k]: theirs[k] for k in ours}
    lines = [re.sub(r'%\d+', lambda m: mapping.get(m[0], m[0]), line) for line in lines]
    caps = sorted(s for s in lines if s.startswith('OpCapability '))
    # All remaining declarations and executable instructions retain their original order.
    consts = {line.split()[0]: line for line in lines if ' = OpConstant ' in line}
    ordered_consts = iter([consts[line.split()[0]] for line in ref if ' = OpConstant ' in line])
    rest = [next(ordered_consts) if ' = OpConstant ' in line else line
            for line in lines if not line.startswith('OpCapability ')]
    return caps + rest


p=argparse.ArgumentParser();p.add_argument('--executable',required=True);p.add_argument('--spirv-tools',default='');a=p.parse_args()
r=Path(__file__).resolve().parent
with tempfile.TemporaryDirectory(prefix='real-ssbo-') as td:
 d=Path(td);binary=d/'shader.bin';reference=d/'reference.spv';prefix=d/'result'
 binary.write_bytes(bytes.fromhex((r/'fixtures/bounded_compute.maxwell.hex').read_text()))
 reference.write_bytes(bytes.fromhex((r/'fixtures/baseline.spirv.hex').read_text()))
 assert hashlib.sha256(binary.read_bytes()).hexdigest()=='72c9756e1e31ebbc00531ff15adfabb53987a342ddb5befbe3e547b65f70516c'
 subprocess.run([a.executable,str(binary),str(prefix),'4'],check=True)
 descriptors=prefix.with_suffix('.descriptors').read_text()
 assert 'offset=0x280 count=1 written=false has_size=false known=true constant=4608' in descriptors
 assert 'offset=0x2A0 count=1 written=false has_size=false known=true constant=4' in descriptors
 lines=descriptors.splitlines()
 expected=[(0x280,False,False,True,4608),(0x2A0,False,False,True,4),(0x310,True,True,False,0),(0x320,True,True,False,0)]
 assert len(lines)==len(expected),'descriptor recognition changed'
 for line,(offset,written,sized,known,constant) in zip(lines,expected):
  b=lambda value:str(value).lower()
  assert line==f'cbuf=0 offset=0x{offset:X} count=1 written={b(written)} has_size={b(sized)} known={b(known)} constant={constant} group=0,0,0 local=0,0,0',line
 tool=lambda n:str(Path(a.spirv_tools)/n) if a.spirv_tools else n
 generated=prefix.with_suffix('.spv')
 for file in [reference,generated]:subprocess.run([tool('spirv-val'),'--target-env','vulkan1.1spv1.4','--uniform-buffer-standard-layout',str(file)],check=True)
 ref=subprocess.check_output([tool('spirv-dis'),'--raw-id',str(reference)],text=True)
 out=subprocess.check_output([tool('spirv-dis'),'--raw-id',str(generated)],text=True)
 assert normalize_to_reference(out,ref)==normalize_to_reference(ref,ref),'executable SPIR-V changed'
 mutation=out.replace('OpIAdd ','OpISub ',1);assert normalize_to_reference(mutation,ref)!=normalize_to_reference(ref,ref)
 print('PASS real captured Maxwell: general range4608, second range4; SPIR-V unchanged modulo inert declarations, arithmetic mutation rejected, spirv-val passed')
