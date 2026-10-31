#!/usr/bin/env python3
from pathlib import Path
import argparse,subprocess,tempfile,os
r=Path(__file__).resolve().parents[2]
p=argparse.ArgumentParser();p.add_argument('--fmt',required=True);p.add_argument('--range',required=True);p.add_argument('--boost',required=True);a=p.parse_args()
base=subprocess.check_output(['git','show','c10731c2d5ab00dbc115e651edbb88e221dcf9cc:ir_opt/global_memory_to_storage_buffer_pass.cpp'],cwd=r,text=True)
def function(s,name):
 start=s.index(name);start=s.rfind('\n',0,start)+1;br=s.index('{',start);end=br+1;depth=1
 while depth:depth+=(s[end]=='{')-(s[end]=='}');end+=1
 return s[start:end]
baseline='namespace Shader::Optimization {\n'+function(base,'std::optional<AffineOffsetRange> TrackOffsetRange').replace('TrackOffsetRange','BaselineTrackOffsetRange')+'\n'+function(base,'ComputeStorageBufferAccessRange CollectAccessRange').replace('CollectAccessRange','BaselineCollectAccessRange').replace('TrackOffsetRange','BaselineTrackOffsetRange')+'\n}'
inc=[r,r/'include',Path(a.fmt)/'include',Path(a.range)/'include']+list((Path(a.boost)/'libs').glob('*/include'))
with tempfile.TemporaryDirectory(prefix='ssbo-ir-') as td:
 d=Path(td);(d/'baseline.inc').write_text(baseline)
 cmd=[os.environ.get('CXX','c++'),'-std=c++20','-O1','-w','-DFMT_HEADER_ONLY','-ffunction-sections','-fdata-sections','-fsanitize=undefined','-fno-sanitize-recover=all',*[f'-I{x}'for x in inc],f'-I{d}',str(Path(__file__).with_name('test.cpp')),*[str(r/'frontend/ir'/x)for x in ['value.cpp','microinstruction.cpp','opcodes.cpp','type.cpp']],'-Wl,--gc-sections','-o',str(d/'test')]
 subprocess.run(cmd,check=True);subprocess.run([d/'test'],check=True)
