// SPDX-License-Identifier: GPL-2.0-or-later
/* Dump the DICE tx/rx channel names and clock-source names, raw, so the
 * depacketiser's channel map is built from what the console says rather than
 * from a guess. Throwaway. */
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/IOCFPlugIn.h>
#include <IOKit/firewire/IOFireWireLib.h>
#include <libkern/OSByteOrder.h>
#include <stdio.h>
static IOFireWireLibDeviceRef d; static io_object_t sv; static int sw;
static UInt32 rq(UInt64 a){FWAddress f={0,(UInt16)(a>>32),(UInt32)a};UInt32 v=0;
  (*d)->ReadQuadlet(d,sv,&f,&v,false,0);return sw?OSSwapInt32(v):v;}
static void str(const char*label,UInt64 base,int bytes){char b[1025]={0};
  for(int i=0;i<bytes/4;i++){UInt32 q=rq(base+i*4);  /* host-order value */
    b[i*4]=q&0xff;b[i*4+1]=(q>>8)&0xff;b[i*4+2]=(q>>16)&0xff;b[i*4+3]=(q>>24)&0xff;}
  printf("%s:\n  ",label);for(int i=0;i<bytes&&b[i];i++)putchar(b[i]>=32&&b[i]<127?b[i]:'.');printf("\n");}
int main(void){io_iterator_t it;IOServiceGetMatchingServices(kIOMainPortDefault,IOServiceMatching("IOFireWireDevice"),&it);
  io_service_t s=IOIteratorNext(it);if(!s){puts("no device");return 1;}
  IOCFPlugInInterface**p;SInt32 sc;IOCreatePlugInInterfaceForService(s,kIOFireWireLibTypeID,kIOCFPlugInInterfaceID,&p,&sc);
  (*p)->QueryInterface(p,CFUUIDGetUUIDBytes(kIOFireWireDeviceInterfaceID_v9),(void**)&d);(*d)->Open(d);sv=(*d)->GetDevice(d);
  FWAddress f={0,0xffff,0xf0000404};UInt32 m;(*d)->ReadQuadlet(d,sv,&f,&m,false,0);sw=(m!=0x31333934);
  UInt64 P=0xffffe0000000ULL,g=P+(UInt64)rq(P)*4,t=P+(UInt64)rq(P+8)*4,r=P+(UInt64)rq(P+16)*4;
  UInt32 tsz=rq(t+4),rsz=rq(r+4);printf("tx block %u quadlets, rx block %u quadlets\n",tsz,rsz);
  str("clock source names (global+0x68)",g+0x68,256);
  str("tx[0] names (tx+0x08+0x10)",t+0x08+0x10,tsz*4-0x10>1024?1024:tsz*4-0x10);
  str("rx[0] names (rx+0x08+0x14? trying +0x10 and +0x14)",r+0x08+0x10,256);
  str("rx[0] names at +0x14",r+0x08+0x14,256);
  (*d)->Close(d);return 0;}
