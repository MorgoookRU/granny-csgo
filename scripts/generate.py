#!/usr/bin/env python3
"""Generate native/Android catalogs and original synthesized weapon sounds."""
import json
import math
import random
import struct
import wave
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

def generate(out: Path):
    weapons = json.loads((ROOT / 'weapons.json').read_text())
    assert len(weapons) == 42 and len({w['name'] for w in weapons}) == 42
    out.mkdir(parents=True, exist_ok=True)
    native = ['#pragma once', '#include "combat.h"', 'static const Weapon weapons[] = {']
    for w in weapons:
        nums = [w['kind'], w['price'], w['mag'], w['reserve'], w.get('pellets', 1)]
        floats = [w[k] for k in ['damage', 'rpm', 'reload', 'spread', 'kick', 'speed']]
        flags = [int(w.get(k, False)) for k in ['auto', 'scope', 'silenced', 'burst', 'shell']]
        assert all(v >= 0 for v in nums) and w['rpm'] > 0
        native.append('{' + json.dumps(w['name']) + ',' + ','.join(map(str, nums)) + ',' +
                      ','.join(f'{float(v):.6f}f' for v in floats) + ',' + ','.join(map(str, flags)) + '},')
    native.extend(['};', 'static constexpr int WEAPON_COUNT = sizeof(weapons)/sizeof(weapons[0]);'])
    (out / 'weapons_generated.h').write_text('\n'.join(native) + '\n')
    java = ['package org.modlab.granny;', 'final class Weapons {']
    for name, key in [('NAMES', 'name'), ('GROUPS', 'group')]:
        java.append('static final String[] ' + name + ' = {' + ','.join(json.dumps(w[key], ensure_ascii=False) for w in weapons) + '};')
    java.append('static final int[] PRICES = {' + ','.join(str(w['price']) for w in weapons) + '};')
    java.append('}')
    (out / 'Weapons.java').write_text('\n'.join(java), encoding='utf-8')
    sounds = out / 'assets' / 'granny_csgo'
    sounds.mkdir(parents=True, exist_ok=True)
    rng = random.Random(2700)
    for kind in range(8):
        frames = []
        duration = 0.16 + 0.025 * kind
        frequency = [160, 200, 110, 70, 80, 95, 900, 1500][kind]
        for i in range(int(22050 * duration)):
            t = i / 22050
            env = math.exp(-t * (25 if kind < 6 else 45))
            noise = rng.uniform(-1, 1) * (0.75 if kind < 6 else 0.2)
            value = (noise + 0.3 * math.sin(2 * math.pi * frequency * t)) * env
            frames.append(struct.pack('<h', int(max(-1, min(1, value)) * 22000)))
        with wave.open(str(sounds / f'shot{kind}.wav'), 'wb') as wav:
            wav.setnchannels(1); wav.setsampwidth(2); wav.setframerate(22050)
            wav.writeframes(b''.join(frames))
    print(f'Generated {len(weapons)} weapons and 8 original sound effects in {out}')

if __name__ == '__main__':
    import sys
    generate(Path(sys.argv[1]))
