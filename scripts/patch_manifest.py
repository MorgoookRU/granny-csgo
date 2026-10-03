"""Edit selected Android binary XML attributes without changing resource IDs.

Retains every existing string-pool index, resource map, and opaque XML chunk.
Adds a personal test application ID/label and makes the merged APK standalone.
"""
import struct
from pathlib import Path

U32 = lambda b, o: struct.unpack_from('<I', b, o)[0]
U16 = lambda b, o: struct.unpack_from('<H', b, o)[0]
ANDROID = 'http://schemas.android.com/apk/res/android'

def length8(b, p):
    n = b[p]; p += 1
    if n & 0x80:
        n = ((n & 0x7f) << 8) | b[p]; p += 1
    return n, p

def length16(b, p):
    n = U16(b, p); p += 2
    if n & 0x8000:
        n = ((n & 0x7fff) << 16) | U16(b, p); p += 2
    return n, p

def encode8(n):
    return bytes([n]) if n < 128 else bytes([0x80 | (n >> 8), n & 255])

def encode16(n):
    return struct.pack('<H', n) if n < 0x8000 else struct.pack('<HH', 0x8000 | (n >> 16), n & 65535)

class StringPool:
    def __init__(self, b):
        self.header_size = U16(b, 2)
        count, styles, self.flags, start, style_start = struct.unpack_from('<5I', b, 8)
        self.styles = styles
        self.style_offsets = b[self.header_size + 4*count:self.header_size + 4*(count+styles)]
        self.style_data = b[style_start:] if style_start else b''
        self.values = []
        for i in range(count):
            p = start + U32(b, self.header_size + i*4)
            if self.flags & 0x100:
                _, p = length8(b, p); n, p = length8(b, p)
                self.values.append(b[p:p+n].decode('utf-8'))
            else:
                n, p = length16(b, p); self.values.append(b[p:p+n*2].decode('utf-16le'))

    def index(self, value):
        if value not in self.values: self.values.append(value)
        return self.values.index(value)

    def build(self):
        offsets, data = [], bytearray()
        for text in self.values:
            offsets.append(len(data)); utf16 = text.encode('utf-16le')
            if self.flags & 0x100:
                utf8 = text.encode('utf-8')
                data.extend(encode8(len(utf16)//2)+encode8(len(utf8))+utf8+b'\0')
            else: data.extend(encode16(len(utf16)//2)+utf16+b'\0\0')
        data.extend(b'\0' * (-len(data) % 4))
        start = 28 + 4*len(offsets) + len(self.style_offsets)
        style_start = start+len(data) if self.style_data else 0
        size = start+len(data)+len(self.style_data)
        return (struct.pack('<HHI5I',1,28,size,len(offsets),self.styles,self.flags & ~1,start,style_start)
                + struct.pack('<'+'I'*len(offsets),*offsets)+self.style_offsets+data+self.style_data)

def patch(data, package='com.modlab.grannycsgo', label='Granny Tactical Lab · 1', version_code=None):
    if U16(data,0)!=3 or U32(data,4)!=len(data):raise ValueError('Invalid binary XML')
    chunks=[]; offset=U16(data,2); pool=None; pool_index=None
    while offset<len(data):
        size=U32(data,offset+4)
        if size<8 or offset+size>len(data):raise ValueError('Invalid XML chunk')
        chunk=bytearray(data[offset:offset+size])
        if U16(chunk,0)==1:
            pool=StringPool(chunk);pool_index=len(chunks)
        chunks.append(chunk);offset+=size
    if pool is None:raise ValueError('Missing string pool')
    pool.values=[s.replace('com.dvloper.granny',package) for s in pool.values]
    empty=pool.index('');label_index=pool.index(label);changes=[]
    for b in chunks:
        if U16(b,0)!=0x102:continue
        header=U16(b,2);tag=pool.values[U32(b,header+4)]
        attr_start=U16(b,header+8);attr_size=U16(b,header+10);attr_count=U16(b,header+12)
        attributes={}
        for i in range(attr_count):
            p=header+attr_start+i*attr_size;attributes[pool.values[U32(b,p+4)]]=p
        def string_attr(name,index):
            p=attributes[name];struct.pack_into('<I',b,p+8,index);b[p+15]=3;struct.pack_into('<I',b,p+16,index);changes.append(tag+'.'+name)
        if tag=='manifest' and 'requiredSplitTypes' in attributes:string_attr('requiredSplitTypes',empty)
        if tag=='manifest' and version_code is not None and 'versionCode' in attributes:
            p=attributes['versionCode'];struct.pack_into('<I',b,p+8,0xffffffff);b[p+15]=0x10;struct.pack_into('<I',b,p+16,version_code);changes.append('manifest.versionCode')
        if tag=='application' and 'label' in attributes:string_attr('label',label_index)
        if tag=='meta-data' and 'name' in attributes:
            p=attributes['name'];name=pool.values[U32(b,p+16)]
            if name=='com.android.vending.splits.required':
                p=attributes['value'];struct.pack_into('<I',b,p+8,0xffffffff);b[p+15]=0x12;struct.pack_into('<I',b,p+16,0);changes.append('splits.required=false')
    if 'splits.required=false' not in changes:raise ValueError('Expected split metadata missing')
    if 'application.label' not in changes:raise ValueError('Expected app label missing')
    chunks[pool_index]=pool.build();result=bytearray(data[:U16(data,2)])+b''.join(chunks)
    struct.pack_into('<I',result,4,len(result))
    return bytes(result),changes

if __name__=='__main__':
    import sys
    source,destination=map(Path,sys.argv[1:3]);result,changes=patch(source.read_bytes());destination.write_bytes(result);print(changes)
