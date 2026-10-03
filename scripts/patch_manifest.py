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

def diagnostic_launcher(data):
    """Keep resource IDs intact while separating the launcher from Unity startup."""
    chunks=[]; offset=U16(data,2)
    while offset<len(data):
        size=U32(data,offset+4); chunks.append(bytearray(data[offset:offset+size])); offset+=size
    pool_index=next(i for i,c in enumerate(chunks) if U16(c,0)==1)
    pool=StringPool(chunks[pool_index]); namespace=pool.index(ANDROID)
    map_index=next(i for i,c in enumerate(chunks) if U16(c,0)==0x180)
    resource_ids=list(struct.unpack_from('<'+'I'*((len(chunks[map_index])-8)//4),chunks[map_index],8))
    ids={'name':0x01010003,'enabled':0x0101000e,'exported':0x01010010,'process':0x01010011}
    def attr(name,value):
        index=pool.index(name)
        while len(resource_ids)<=index:resource_ids.append(0)
        resource_ids[index]=ids[name]
        if isinstance(value,bool): raw=0xffffffff; kind=0x12; typed=int(value)
        else:raw=pool.index(value);kind=3;typed=raw
        return struct.pack('<IIIHBBI',namespace,index,raw,8,0,kind,typed)
    def start(tag,attributes=()):
        tag_index=pool.index(tag); values=[attr(k,v) for k,v in attributes]
        values.sort(key=lambda a:resource_ids[U32(a,4)])
        return bytearray(struct.pack('<HHIII',0x102,16,36+20*len(values),0,0xffffffff)+
            struct.pack('<IIHHHHHH',0xffffffff,tag_index,20,20,len(values),0,0,0)+b''.join(values))
    def end(tag):
        return bytearray(struct.pack('<HHIIIII',0x103,16,24,0,0xffffffff,0xffffffff,pool.index(tag)))
    def attributes(chunk):
        h=U16(chunk,2);begin=h+U16(chunk,h+8);stride=U16(chunk,h+10)
        return {pool.values[U32(chunk,p+4)]:p for p in range(begin,begin+stride*U16(chunk,h+12),stride)}
    def set_attr(chunk,name,value):
        attrs=attributes(chunk); replacement=attr(name,value)
        if name in attrs:chunk[attrs[name]:attrs[name]+20]=replacement
        else:
            h=U16(chunk,2);count=U16(chunk,h+12);chunk.extend(replacement)
            struct.pack_into('<H',chunk,h+12,count+1);struct.pack_into('<I',chunk,4,len(chunk))
        # aapt stores Android attributes ordered by resource ID.
        h=U16(chunk,2);begin=h+U16(chunk,h+8);count=U16(chunk,h+12)
        values=[chunk[begin+i*20:begin+(i+1)*20] for i in range(count)]
        values.sort(key=lambda a:resource_ids[U32(a,4)] if U32(a,4)<len(resource_ids) else 0)
        chunk[begin:begin+20*count]=b''.join(values)
    def value(chunk,name):
        p=attributes(chunk).get(name)
        return pool.values[U32(chunk,p+16)] if p is not None and chunk[p+15]==3 else None
    output=[];unity=False;skip_depth=0
    for chunk in chunks:
        kind=U16(chunk,0)
        tag=pool.values[U32(chunk,20)] if kind in (0x102,0x103) else ''
        if skip_depth:
            if kind==0x102:skip_depth+=1
            elif kind==0x103:skip_depth-=1
            continue
        if kind==0x102:
            if tag=='application':set_attr(chunk,'name','org.modlab.granny.LabApplication')
            if tag=='provider':set_attr(chunk,'enabled',False)
            if tag=='activity':
                unity=value(chunk,'name')=='com.unity3d.player.UnityPlayerActivity'
                if unity:set_attr(chunk,'process',':game');set_attr(chunk,'exported',False)
            if unity and tag=='intent-filter':skip_depth=1;continue
        if kind==0x103 and tag=='activity':unity=False
        if kind==0x103 and tag=='application':
            output.extend([start('activity',[('name','org.modlab.granny.DiagnosticActivity'),('exported',True)]),
                start('intent-filter'),start('action',[('name','android.intent.action.MAIN')]),end('action'),
                start('category',[('name','android.intent.category.LAUNCHER')]),end('category'),end('intent-filter'),end('activity')])
        output.append(chunk)
    output[pool_index]=pool.build()
    output[map_index]=bytearray(struct.pack('<HHI',0x180,8,8+4*len(resource_ids))+struct.pack('<'+'I'*len(resource_ids),*resource_ids))
    result=bytearray(data[:U16(data,2)])+b''.join(output);struct.pack_into('<I',result,4,len(result))
    return bytes(result)

if __name__=='__main__':
    import sys
    source,destination=map(Path,sys.argv[1:3]);result,changes=patch(source.read_bytes());destination.write_bytes(result);print(changes)
