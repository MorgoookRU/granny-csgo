package org.modlab.granny;
import java.io.*;
import java.util.Base64;
import java.util.zip.GZIPOutputStream;
public final class TombstoneCheck {
 public static void main(String[] args) throws Exception {
  byte[] data=Base64.getDecoder().decode("KMPsATDI7AFSIwgLEgdTSUdTRUdWIgtTRUdWX01BUEVSUkj/////j4CAgPABchZuYXRpdmUgZnJhbWUgYXZhaWxhYmxlggE1CMPsARIvCMPsARIEbWFpbiIjCLQkIg1jaGVja2VkSW52b2tlKCoyB2xpYmMuc29CBGFiMTKCAUQIyOwBEj4IyOwBEglVbml0eU1haW4iLQi0JCINY2hlY2tlZEludm9rZSgqMhFsaWJncmFubnlfY3Nnby5zb0IEYWIxMso+A3h5eg==");
  String report=NativeTombstone.decode(data);
  if(!report.contains("pid=30275 tid=30280")||!report.contains("SIGSEGV SEGV_MAPERR")||!report.contains("fault_address=0xf0000000ffffffff")) throw new AssertionError(report);
  if(!report.contains("CRASHING THREAD 30280 UnityMain")||!report.contains("pc 0000000000001234 libgranny_csgo.so (checkedInvoke+42) [build_id=ab12]")||report.contains("libc.so")) throw new AssertionError(report);
  ByteArrayOutputStream out=new ByteArrayOutputStream();
  try(GZIPOutputStream gzip=new GZIPOutputStream(out)){gzip.write(data);}
  if(!NativeTombstone.read(new ByteArrayInputStream(out.toByteArray())).equals(report)) throw new AssertionError("gzip mismatch");
  byte[] incomplete=java.util.Arrays.copyOf(data,data.length-1);
  if(!NativeTombstone.decode(incomplete).contains("tombstone truncated")) throw new AssertionError("truncation not reported");
  try{NativeTombstone.decode(new byte[]{0});throw new AssertionError("invalid tag accepted");}catch(IOException expected){}
  System.out.println("Tombstone checks passed: official protobuf fixture, crashing thread, unsigned address, gzip, unknown field, truncation, invalid tag");
 }
}
