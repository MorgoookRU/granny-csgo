"""Validate native hook targets against the original ELF and a method dump.

Method addresses come from Il2CppDumper's script.json (virtual addresses) or from a
dump.cs-style listing whose "// RVA 0x..." values are file offsets (LibCpp2IL), which are
converted to virtual addresses through the executable PT_LOAD segment.
"""
import argparse
import json
import re
import struct
from pathlib import Path

TARGETS = [('fixedUpdate', 'fixedBytes', 'FPSControllerNEW', 'FixedUpdate', '()'),
           ('menuStart', 'menuBytes', 'MenuImageSwitcher', 'Start', '()'),
           ('characterMove', 'moveBytes', 'UnityEngine.CharacterController', 'Move', '(UnityEngine.Vector3 ')]


def method_addresses(lab, dump, segments):
    script = lab / 'tools/il2cppdumper/script.json' if lab else None
    if script and script.exists():
        methods = json.loads(script.read_text())['ScriptMethod']
        names = {'FPSControllerNEW': 'FPSControllerNEW', 'MenuImageSwitcher': 'MenuImageSwitcher', 'UnityEngine.CharacterController': 'UnityEngine.CharacterController'}
        return {(cls, name): next(x['Address'] for x in methods if x['Name'] == f'{names[cls]}$${name}') for _, _, cls, name, _ in TARGETS}
    text = dump.read_text(errors='replace')
    code = next(s for s in segments if s[0] == 1 and s[1] & 1)
    result = {}
    for _, _, cls, name, params in TARGETS:
        block = re.search(r'^class ' + re.escape(cls) + r' [^\n]*\{\n(.*?)^\}', text, re.S | re.M).group(1)
        offset = int(re.search(r' ' + re.escape(name) + re.escape(params) + r'[^\n]*// RVA 0x([0-9A-F]+)', block).group(1), 16)
        result[(cls, name)] = offset - code[2] + code[3]
    return result


def check(elf_path, lab, dump):
    root = Path(__file__).resolve().parents[1]
    header = (root / 'native/target_layout.h').read_text()

    def number(name):
        return int(re.search(r'\b' + name + r'=(0x[0-9a-f]+);', header)[1], 16)

    def data(name):
        text = re.search(r'\b' + name + r'\[\]=\{([^}]+)\}', header)[1]
        return bytes(int(x.strip(), 16) for x in text.split(','))
    elf = elf_path.read_bytes()
    assert elf[:6] == b'\x7fELF\x02\x01', 'Expected 64-bit little-endian ELF'
    assert struct.unpack_from('<H', elf, 18)[0] == 183, 'Expected ARM64'
    offset = struct.unpack_from('<Q', elf, 32)[0]
    stride, count = struct.unpack_from('<HH', elf, 54)
    segments = [struct.unpack_from('<IIQQQQQQ', elf, offset + i * stride) for i in range(count)]
    builds = []
    for kind, flags, pos, vaddr, physical, size, memory, align in segments:
        if kind != 4:
            continue
        end = pos + size
        while pos + 12 <= end:
            namesz, descsz, typ = struct.unpack_from('<III', elf, pos); pos += 12
            name = elf[pos:pos + namesz]; pos += (namesz + 3) & ~3
            desc = elf[pos:pos + descsz]; pos += (descsz + 3) & ~3
            if typ == 3 and name == b'GNU\0':
                builds.append(desc)
    assert data('buildId') in builds, 'Original build ID differs from startup target'
    addresses = method_addresses(lab, dump, segments)
    for field, bytes_field, cls, method, _ in TARGETS:
        address = number(field)
        assert addresses[(cls, method)] == address, f'{cls}.{method} address differs from method dump'
        segment = next(s for s in segments if s[0] == 1 and s[1] & 1 and s[3] <= address < s[3] + s[5])
        pos = segment[2] + address - segment[3]; expected = data(bytes_field)
        assert elf[pos:pos + len(expected)] == expected, f'{cls}.{method} entry bytes differ'
    print('Hook target checks passed: ARM64, original build ID, FixedUpdate, MenuImageSwitcher.Start and CharacterController.Move addresses and entry bytes')


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--lab', type=Path, help='legacy lab with libil2cpp.so and tools/il2cppdumper/script.json')
    parser.add_argument('--elf', type=Path, help='original libil2cpp.so')
    parser.add_argument('--dump', type=Path, help='dump.cs with file-offset RVAs')
    a = parser.parse_args()
    check(a.elf or a.lab / 'libil2cpp.so', a.lab, a.dump)
