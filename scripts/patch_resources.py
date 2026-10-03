"""Rename a compiled resource package without renumbering any resource IDs."""
import struct


def rename_package(data, original='com.dvloper.granny', package='com.modlab.grannycsgo'):
    result = bytearray(data)
    if len(result) < 12:
        raise ValueError('Truncated resource table')
    kind, header, size = struct.unpack_from('<HHI', result)
    if kind != 0x0002 or size != len(result) or header < 12:
        raise ValueError('Invalid resource table header')
    encoded = package.encode('utf-16le')
    if len(encoded) > 254 or '\0' in package:
        raise ValueError('Resource package name exceeds the fixed header field')
    offset = header
    changes = []
    while offset < size:
        if offset + 8 > size:
            raise ValueError('Truncated resource chunk')
        chunk_kind, chunk_header, chunk_size = struct.unpack_from('<HHI', result, offset)
        if chunk_size < chunk_header or chunk_header < 8 or offset + chunk_size > size:
            raise ValueError('Invalid resource chunk bounds')
        if chunk_kind == 0x0200:
            if chunk_header < 268:
                raise ValueError('Truncated package header')
            name_bytes = bytes(result[offset + 12:offset + 268])
            name = name_bytes.decode('utf-16le').split('\0', 1)[0]
            if name == original:
                result[offset + 12:offset + 268] = encoded + bytes(256 - len(encoded))
                changes.append((name, package))
            elif name != package:
                raise ValueError('Unexpected resource package: ' + name)
        offset += chunk_size
    if len(changes) != 1:
        raise ValueError('Expected exactly one original resource package')
    return bytes(result), changes
