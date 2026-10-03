"""Validate native startup hook targets against the original ELF and method dump."""
import argparse
import json
import re
import struct
from pathlib import Path


def check(lab):
    root=Path(__file__).resolve().parents[1]
    header=(root/'native/target_layout.h').read_text()
    def number(name):
        return int(re.search(r'\b'+name+r'=(0x[0-9a-f]+);',header)[1],16)
    def data(name):
        text=re.search(r'\b'+name+r'\[\]=\{([^}]+)\}',header)[1]
        return bytes(int(x.strip(),16) for x in text.split(','))
    elf=(lab/'libil2cpp.so').read_bytes()
    assert elf[:6]==b'\x7fELF\x02\x01', 'Expected 64-bit little-endian ELF'
    assert struct.unpack_from('<H',elf,18)[0]==183, 'Expected ARM64'
    offset=struct.unpack_from('<Q',elf,32)[0]
    stride,count=struct.unpack_from('<HH',elf,54)
    segments=[struct.unpack_from('<IIQQQQQQ',elf,offset+i*stride) for i in range(count)]
    builds=[]
    for kind,flags,pos,vaddr,physical,size,memory,align in segments:
        if kind!=4: continue
        end=pos+size
        while pos+12<=end:
            namesz,descsz,typ=struct.unpack_from('<III',elf,pos);pos+=12
            name=elf[pos:pos+namesz];pos+=(namesz+3)&~3
            desc=elf[pos:pos+descsz];pos+=(descsz+3)&~3
            if typ==3 and name==b'GNU\0':builds.append(desc)
    assert data('buildId') in builds, 'Original build ID differs from startup target'
    methods=json.loads((lab/'tools/il2cppdumper/script.json').read_text())['ScriptMethod']
    for field,bytes_field,method in [('fixedUpdate','fixedBytes','FPSControllerNEW$$FixedUpdate'),('menuStart','menuBytes','MenuImageSwitcher$$Start')]:
        address=number(field)
        entry=next(x for x in methods if x['Name']==method)
        assert entry['Address']==address, method+' RVA differs from method dump'
        segment=next(s for s in segments if s[0]==1 and s[1]&1 and s[3]<=address< s[3]+s[5])
        pos=segment[2]+address-segment[3];expected=data(bytes_field)
        assert elf[pos:pos+len(expected)]==expected, method+' entry bytes differ'
    print('Startup hook checks passed: ARM64, original build ID, both method RVAs and executable entry bytes')


if __name__=='__main__':
    parser=argparse.ArgumentParser();parser.add_argument('--lab',type=Path,default=Path('/workspace/granny-lab'))
    check(parser.parse_args().lab)
