"""Exercise Granny's original ARM64 object invoker without an Android device.

Requires Unicorn (uv run --with unicorn python checks/unity_object_check.py).
This checks the real invoker and Object.op_Implicit instructions, rather than a
reimplementation of their behavior. It does not emulate Unity gameplay.
"""
import argparse
import struct
from pathlib import Path

from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM
from unicorn.arm64_const import (
    UC_ARM64_REG_SP, UC_ARM64_REG_LR, UC_ARM64_REG_X0, UC_ARM64_REG_X1,
    UC_ARM64_REG_X2, UC_ARM64_REG_X3, UC_ARM64_REG_X4,
)


class Target:
    def __init__(self, lab):
        self.elf = (lab / 'libil2cpp.so').read_bytes()
        self.metadata = (lab / 'global-metadata.dat').read_bytes()
        self.header = struct.unpack_from('<64I', self.metadata)
        assert self.header[:2] == (0xFAB11BAF, 31)
        data = self.elf
        pos = struct.unpack_from('<Q', data, 32)[0]
        stride, count = struct.unpack_from('<HH', data, 54)
        self.loads = [s for i in range(count)
                      if (s := struct.unpack_from('<IIQQQQQQ', data, pos + i * stride))[0] == 1]
        pos = struct.unpack_from('<Q', data, 40)[0]
        stride, count = struct.unpack_from('<HH', data, 58)
        self.relocations = {}
        for i in range(count):
            section = struct.unpack_from('<IIQQQQIIQQ', data, pos + i * stride)
            if section[1] != 4:
                continue
            for j in range(section[4], section[4] + section[5], section[9]):
                address, kind, value = struct.unpack_from('<QQq', data, j)
                if kind & 0xFFFFFFFF == 1027:  # R_AARCH64_RELATIVE
                    self.relocations[address] = value

    def offset(self, address):
        return next(pos + address - va for _, _, pos, va, _, size, _, _ in self.loads
                    if va <= address < va + size)

    def pointer(self, address):
        return self.relocations.get(address, struct.unpack_from('<Q', self.elf, self.offset(address))[0])

    def string(self, index):
        pos = self.header[6] + index
        return self.metadata[pos:self.metadata.index(b'\0', pos)].decode()

    def type_definition(self, index):
        return struct.unpack_from('<16i8HII', self.metadata, self.header[40] + index * 88)

    def method(self, index):
        return struct.unpack_from('<7I4H', self.metadata, self.header[12] + index * 36)


def check(lab):
    target = Target(lab)
    metadata_registration, code_registration = 0x2E62EB0, 0x2D74C28
    sizes = target.pointer(metadata_registration + 104)
    types = target.pointer(metadata_registration + 56)
    for index, name, expected in [(151, 'Boolean', 1), (259, 'Single', 4), (5949, 'Vector3', 12)]:
        definition = target.type_definition(index)
        assert target.string(definition[0]) == name
        assert definition[-2] & 1, name + ' must be a value type'
        size_record = target.pointer(sizes + index * 8)
        instance_size = struct.unpack_from('<I', target.elf, target.offset(size_record))[0]
        assert instance_size - 16 == expected, name + ' boxed payload layout changed'
        type_pointer = target.pointer(types + definition[2] * 8)
        bits = struct.unpack_from('<I', target.elf, target.offset(type_pointer) + 8)[0]
        assert bits & 0x80000000, name + ' value type flag changed'

    modules = target.pointer(code_registration + 128)
    count = target.pointer(code_registration + 120)
    module = None
    for i in range(count):
        entry = target.pointer(modules + i * 8)
        pos = target.offset(target.pointer(entry))
        name = target.elf[pos:target.elf.index(b'\0', pos)].decode()
        if name == 'UnityEngine.CoreModule.dll':
            module = entry
            break
    assert module is not None
    definition = target.type_definition(6057)
    assert target.string(definition[0]) == 'Object'
    method = next(target.method(i) for i in range(definition[9], definition[9] + definition[16])
                  if target.string(target.method(i)[0]) == 'op_Implicit')
    rid = (method[6] & 0xFFFFFF) - 1
    pointer = target.pointer(target.pointer(module + 16) + rid * 8)
    invoker_index = struct.unpack_from('<i', target.elf, target.offset(target.pointer(module + 40)) + rid * 4)[0]
    invoker = target.pointer(target.pointer(code_registration + 48) + invoker_index * 8)
    assert pointer == 0x2AA2EC0 and invoker == 0x12B4770

    cpu = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
    base = min(s[3] for s in target.loads) & ~0xFFF
    end = (max(s[3] + s[6] for s in target.loads) + 0xFFF) & ~0xFFF
    memory = bytearray(end - base)
    cpu.mem_map(base, len(memory))
    for _, _, pos, va, _, size, _, _ in target.loads:
        memory[va - base:va - base + size] = target.elf[pos:pos + size]
    for address, value in target.relocations.items():
        struct.pack_into('<Q', memory, address - base, value)
    cpu.mem_write(base, bytes(memory))

    scratch, stack = 0x5000000, 0x7000000
    cpu.mem_map(scratch, 0x10000)
    cpu.mem_map(stack, 0x10000)
    klass, obj, args, result, stop = scratch + 0x100, scratch + 0x1000, scratch + 0x2000, scratch + 0x2100, scratch + 0x3000
    # The class is already initialized in the real gameplay callback. Supply the
    # same initialized state so this small emulation doesn't initialize Unity.
    cpu.mem_write(0x30E0037, b'\1\1')
    cpu.mem_write(0x2EEF500, struct.pack('<Q', scratch))
    cpu.mem_write(scratch, struct.pack('<Q', klass))
    cpu.mem_write(klass + 0xE4, struct.pack('<I', 1))
    cpu.mem_write(obj, struct.pack('<QQQ', klass, 0, 0x12345678))

    def invoke(object_pointer):
        cpu.mem_write(args, struct.pack('<Q', object_pointer))
        cpu.mem_write(result, b'\xA5')
        for register, value in [(UC_ARM64_REG_SP, stack + 0xFFF0), (UC_ARM64_REG_LR, stop),
                                (UC_ARM64_REG_X0, pointer), (UC_ARM64_REG_X1, scratch + 0x4000),
                                (UC_ARM64_REG_X2, 0), (UC_ARM64_REG_X3, args), (UC_ARM64_REG_X4, result)]:
            cpu.reg_write(register, value)
        cpu.emu_start(invoker, stop, count=1000)
        assert cpu.reg_read(UC_ARM64_REG_LR) == stop
        return cpu.mem_read(result, 1)[0]

    assert invoke(obj) == 1, 'Live Unity object must pass the original invoker'
    cpu.mem_write(obj + 0x10, struct.pack('<Q', 0))
    assert invoke(obj) == 0, 'Destroyed Unity object must fail the original invoker'
    assert invoke(0) == 0, 'Null Unity object must fail the original invoker'
    print('Original ARM64 invoker checks passed: direct references; live/destroyed/null objects; Boolean/Single/Vector3 boxed layouts')


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--lab', type=Path, default=Path('/workspace/granny-lab'))
    check(parser.parse_args().lab)
