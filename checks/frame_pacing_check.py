"""Check the libunity code that the native frame-pacing fix relies on.

The mod resolves the registered icall "UnityEngine.Time::get_fixedDeltaTime", checks that it
calls GetTimeManager and reads the RationalTime at TimeManager+0x50, and only then writes
the fixed step. This check finds that getter in the original libunity.so and verifies the
same instruction words and the GetTimeManager stub that the native code expects.
"""
import argparse
import re
import struct
from pathlib import Path


def check(libunity):
    root = Path(__file__).resolve().parents[1]
    header = (root / 'native/target_layout.h').read_text()
    getter = [int(x, 16) if x.strip() != '0' else None for x in re.search(r'fixedGetter\[\]=\{([^}]+)\}', header)[1].split(',')]
    manager = int(re.search(r'timeManagerGetter=(0x[0-9a-f]+);', header)[1], 16)
    elf = libunity.read_bytes()
    offset = struct.unpack_from('<Q', elf, 32)[0]
    stride, count = struct.unpack_from('<HH', elf, 54)
    segments = [struct.unpack_from('<IIQQQQQQ', elf, offset + i * stride) for i in range(count)]
    code = next(s for s in segments if s[0] == 1 and s[1] & 1)
    start, size = code[2], code[5]
    words = struct.unpack_from('<%dI' % (size // 4), elf, start)
    matches = []
    for i in range(len(words) - 4):
        if words[i] != getter[0] or words[i + 2] != getter[2] or words[i + 3] != getter[3] or words[i + 1] >> 26 != 0x25:
            continue
        imm = words[i + 1] & 0x3ffffff
        if imm & 0x2000000:
            imm -= 0x4000000
        target = i + 1 + imm
        if 0 <= target < len(words) and words[target] == manager:
            matches.append(code[3] + i * 4)
    assert matches, 'fixedDeltaTime getter pattern not found in libunity'
    print('Frame pacing check passed: get_fixedDeltaTime -> GetTimeManager(7) -> RationalTime at +0x50;',
          'getter at', ', '.join(hex(m) for m in matches))


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--libunity', type=Path, required=True)
    check(parser.parse_args().libunity)
