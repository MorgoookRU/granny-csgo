package org.modlab.granny;

import java.io.*;
import java.nio.charset.StandardCharsets;
import java.util.*;

/** Reads the public debuggerd tombstone protobuf without generated dependencies.
 * Field numbers: AOSP system/core/debuggerd/proto/tombstone.proto.
 */
final class NativeTombstone {
    private static final int LIMIT = 8 * 1024 * 1024;
    private static final class Field {
        int number, wire; long value; byte[] bytes;
        String text() { return bytes == null ? "" : new String(bytes,StandardCharsets.UTF_8); }
    }
    private static final class Reader {
        final byte[] data; int offset;
        Reader(byte[] data) { this.data=data; }
        long varint() throws IOException {
            long value=0;
            for(int shift=0;shift<64;shift+=7) {
                if(offset>=data.length) throw new EOFException();
                int b=data[offset++]&255; value |= (long)(b&127)<<shift;
                if((b&128)==0) return value;
            }
            throw new IOException("Invalid protobuf varint");
        }
        Field next() throws IOException {
            if(offset==data.length) return null;
            long tag=varint(); Field f=new Field(); f.number=(int)(tag>>>3); f.wire=(int)(tag&7);
            if(f.number==0) throw new IOException("Invalid protobuf field");
            if(f.wire==0) f.value=varint();
            else if(f.wire==2) {
                long n=varint(); if(n<0||n>data.length-offset) throw new EOFException();
                f.bytes=Arrays.copyOfRange(data,offset,offset+(int)n); offset+=(int)n;
            } else if(f.wire==1||f.wire==5) {
                int n=f.wire==1?8:4; if(n>data.length-offset) throw new EOFException(); offset+=n;
            } else throw new IOException("Unsupported protobuf wire type");
            return f;
        }
    }
    private static final class ThreadInfo {
        long id; String name=""; final List<String> frames=new ArrayList<>();
    }
    private static String frame(byte[] data,int index) throws IOException {
        long pc=0,offset=0; String name="",file="",build="";
        Reader reader=new Reader(data); Field f;
        while((f=reader.next())!=null) {
            if(f.number==1) pc=f.value;
            else if(f.number==4) name=f.text();
            else if(f.number==5) offset=f.value;
            else if(f.number==6) file=f.text();
            else if(f.number==8) build=f.text();
        }
        return String.format(Locale.ROOT,"#%02d pc %016x %s%s%s\n",index,pc,file,
            name.isEmpty()?"":" ("+name+"+"+offset+")",build.isEmpty()?"":" [build_id="+build+"]");
    }
    private static ThreadInfo thread(byte[] data) throws IOException {
        ThreadInfo thread=new ThreadInfo(); Reader reader=new Reader(data); Field f;
        while((f=reader.next())!=null) {
            if(f.number==1) thread.id=f.value;
            else if(f.number==2) thread.name=f.text();
            else if(f.number==4&&f.bytes!=null&&thread.frames.size()<64) thread.frames.add(frame(f.bytes,thread.frames.size()));
        }
        return thread;
    }
    private static ThreadInfo mapEntry(byte[] data) throws IOException {
        Reader reader=new Reader(data); Field f; long id=0; ThreadInfo result=null;
        while((f=reader.next())!=null) {
            if(f.number==1) id=f.value;
            else if(f.number==2&&f.bytes!=null) result=thread(f.bytes);
        }
        if(result!=null&&result.id==0) result.id=id;
        return result;
    }
    private static String signal(byte[] data) throws IOException {
        Reader reader=new Reader(data); Field f; long number=0,address=0; String name="",code="";
        while((f=reader.next())!=null) {
            if(f.number==1) number=f.value;
            else if(f.number==2) name=f.text();
            else if(f.number==4) code=f.text();
            else if(f.number==9) address=f.value;
        }
        return "signal="+number+" "+name+" "+code+" fault_address=0x"+Long.toHexString(address)+"\n";
    }
    static String read(InputStream stream) throws IOException {
        if(stream==null) return "(нет нативной трассировки)\n";
        byte[] data;
        BufferedInputStream buffered=new BufferedInputStream(stream);buffered.mark(2);
        int first=buffered.read(),second=buffered.read();buffered.reset();
        InputStream decoded=first==0x1f&&second==0x8b?new java.util.zip.GZIPInputStream(buffered):buffered;
        try(InputStream input=decoded; ByteArrayOutputStream output=new ByteArrayOutputStream()) {
            byte[] buffer=new byte[8192]; int n;
            while(output.size()<LIMIT&&(n=input.read(buffer,0,Math.min(buffer.length,LIMIT-output.size())))>0)
                output.write(buffer,0,n);
            data=output.toByteArray();
        }
        return decode(data);
    }
    static String decode(byte[] data) throws IOException {
        Reader reader=new Reader(data); Field f; long pid=0,tid=0; String signal="",abort="";
        List<ThreadInfo> threads=new ArrayList<>(); StringBuilder causes=new StringBuilder(); boolean truncated=false;
        try {
            while((f=reader.next())!=null) {
                if(f.number==5) pid=f.value;
                else if(f.number==6) tid=f.value;
                else if(f.number==10&&f.bytes!=null) signal=signal(f.bytes);
                else if(f.number==14) abort=f.text();
                else if(f.number==15&&f.bytes!=null) {
                    Reader cause=new Reader(f.bytes); Field c;
                    while((c=cause.next())!=null) if(c.number==1) causes.append("cause: ").append(c.text()).append('\n');
                } else if(f.number==16&&f.bytes!=null&&threads.size()<256) {
                    ThreadInfo t=mapEntry(f.bytes); if(t!=null) threads.add(t);
                }
            }
        } catch(EOFException partial) { truncated=true; }
        StringBuilder out=new StringBuilder("NATIVE TOMBSTONE pid="+pid+" tid="+tid+"\n").append(signal);
        if(!abort.isEmpty()) out.append("abort: ").append(abort).append('\n');
        out.append(causes);
        ThreadInfo crashed=null; for(ThreadInfo t:threads) if(t.id==tid) { crashed=t; break; }
        if(crashed!=null) {
            out.append("CRASHING THREAD ").append(crashed.id).append(' ').append(crashed.name).append('\n');
            for(String line:crashed.frames) out.append(line);
        } else {
            out.append("(crashing thread not found; available stacks)\n"); int count=0;
            for(ThreadInfo t:threads) if(!t.frames.isEmpty()&&count++<4) {
                out.append("THREAD ").append(t.id).append(' ').append(t.name).append('\n');
                for(String line:t.frames) out.append(line);
            }
        }
        if(truncated) out.append("(tombstone truncated at read limit)\n");
        return out.toString();
    }
}
