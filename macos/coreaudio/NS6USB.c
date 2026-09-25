#include "NS6USB.h"
#include "NS6Transport.h"
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOCFPlugIn.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/usb/IOUSBLib.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define VID 0x15e4
#define PID 0x0079
#define RATE 44100
#define FRAME_BYTES 12
#define PACKETS 32
#define SLOT_COUNT 8
#define MAX_PACKET 156
#define STARTUP_FRAMES 2048
#define STARTUP_WAIT_MS 100
#define QUEUE_TARGET_FRAMES 4096
#define CONTROL_SLOTS 16
#define MAX_RATE_ADJUST_Q16 4096

struct slot { IOUSBLowLatencyIsocFrame *frames; unsigned char *data; };
static IOUSBInterfaceInterface **interface;
static IOUSBInterfaceInterface **auxiliary_interface;
static CFRunLoopSourceRef source;
static CFRunLoopRef run_loop;
static struct slot slots[SLOT_COUNT];
static pthread_t thread;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed = PTHREAD_COND_INITIALIZER;
static atomic_bool running;
static bool thread_created, initialized;
static uint64_t fraction;
static int32_t rate_adjust_q16;
static unsigned control_slots;

static int property_u16(io_registry_entry_t entry, CFStringRef key, UInt16 *out) {
    CFTypeRef value=IORegistryEntryCreateCFProperty(entry,key,kCFAllocatorDefault,0);
    int ok=value&&CFGetTypeID(value)==CFNumberGetTypeID()&&CFNumberGetValue(value,kCFNumberSInt16Type,out);
    if(value)CFRelease(value); return ok;
}
static bool is_ns6_interface(io_registry_entry_t service){
    io_registry_entry_t entry=service;
    for(;;){
        UInt16 vendor=0,product=0;
        if(property_u16(entry,CFSTR(kUSBVendorID),&vendor)&&property_u16(entry,CFSTR(kUSBProductID),&product)&&vendor==VID&&product==PID){if(entry!=service)IOObjectRelease(entry);return true;}
        io_registry_entry_t parent=IO_OBJECT_NULL;
        if(IORegistryEntryGetParentEntry(entry,kIOServicePlane,&parent)!=KERN_SUCCESS){if(entry!=service)IOObjectRelease(entry);return false;}
        if(entry!=service)IOObjectRelease(entry); entry=parent;
    }
}
static IOUSBInterfaceInterface **open_interface_once(UInt8 wanted_number) {
    io_iterator_t iterator=IO_OBJECT_NULL; io_service_t service;
    if(IOServiceGetMatchingServices(kIOMainPortDefault,IOServiceMatching("IOUSBHostInterface"),&iterator)!=KERN_SUCCESS)return NULL;
    while((service=IOIteratorNext(iterator))){
        UInt8 number=255;
        if(!is_ns6_interface(service)){IOObjectRelease(service);continue;}
        IOCFPlugInInterface **plugin=NULL; IOUSBInterfaceInterface **candidate=NULL; SInt32 score=0;
        HRESULT result=IOCreatePlugInInterfaceForService(service,kIOUSBInterfaceUserClientTypeID,kIOCFPlugInInterfaceID,&plugin,&score);
        if(result==kIOReturnSuccess&&plugin){result=(*plugin)->QueryInterface(plugin,CFUUIDGetUUIDBytes(kIOUSBInterfaceInterfaceID),(LPVOID)&candidate);IODestroyPlugInInterface(plugin);} IOObjectRelease(service);
        if(result||!candidate)continue; (*candidate)->GetInterfaceNumber(candidate,&number);
        if(number==wanted_number){IOObjectRelease(iterator);return candidate;} (*candidate)->Release(candidate);
    }
    IOObjectRelease(iterator); return NULL;
}
static bool configure_ns6_device(void){
    io_iterator_t iterator=IO_OBJECT_NULL; io_service_t service;
    if(IOServiceGetMatchingServices(kIOMainPortDefault,IOServiceMatching("IOUSBHostDevice"),&iterator)!=KERN_SUCCESS)return false;
    while((service=IOIteratorNext(iterator))){
        UInt16 vendor=0,product=0;
        if(!property_u16(service,CFSTR(kUSBVendorID),&vendor)||!property_u16(service,CFSTR(kUSBProductID),&product)||vendor!=VID||product!=PID){IOObjectRelease(service);continue;}
        IOCFPlugInInterface **plugin=NULL; IOUSBDeviceInterface **device=NULL; SInt32 score=0;
        HRESULT result=IOCreatePlugInInterfaceForService(service,kIOUSBDeviceUserClientTypeID,kIOCFPlugInInterfaceID,&plugin,&score);
        if(result==kIOReturnSuccess&&plugin){result=(*plugin)->QueryInterface(plugin,CFUUIDGetUUIDBytes(kIOUSBDeviceInterfaceID),(LPVOID)&device);IODestroyPlugInInterface(plugin);} IOObjectRelease(service); IOObjectRelease(iterator);
        if(result||!device)return false;
        IOReturn opened=(*device)->USBDeviceOpenSeize(device);
        IOReturn configured=opened==kIOReturnSuccess?(*device)->SetConfiguration(device,1):opened;
        if(opened==kIOReturnSuccess)(*device)->USBDeviceClose(device);
        (*device)->Release(device); return configured==kIOReturnSuccess;
    }
    IOObjectRelease(iterator); return false;
}
static IOUSBInterfaceInterface **open_interface(void){
    IOUSBInterfaceInterface **candidate=open_interface_once(0); if(candidate)return candidate;
    if(!configure_ns6_device())return NULL;
    const struct timespec delay={0,100000000}; nanosleep(&delay,NULL);
    return open_interface_once(0);
}
static IOReturn activate_controller(void){
    static const unsigned char sysex[]={0xf0,0x00,0x01,0x3f,0x00,0x79,0x51,0x00,0x10,0x49,0x01,0x08,0x01,0x01,0x08,0x04,0x0c,0x0d,0x01,0x0a,0x0a,0x05,0x06,0x05,0x0d,0x07,0x0e,0x08,0x07,0x0d,0xf7};
    unsigned char packet[42]; memset(packet,0xfd,sizeof(packet)); memcpy(packet,sysex,sizeof(sysex)); packet[41]=0;
    UInt8 endpoints=0; IOReturn result=(*interface)->GetNumEndpoints(interface,&endpoints); if(result!=kIOReturnSuccess)return result;
    for(UInt8 pipe=1;pipe<=endpoints;++pipe){
        UInt8 direction=0,number=0,type=0,interval=0; UInt16 maximum=0;
        result=(*interface)->GetPipeProperties(interface,pipe,&direction,&number,&type,&maximum,&interval);
        if(result==kIOReturnSuccess&&direction==kUSBOut&&number==4)return (*interface)->WritePipe(interface,pipe,packet,sizeof(packet));
    }
    return kIOReturnNotFound;
}
static bool usb_failure(const char *operation,IOReturn result){fprintf(stderr,"Numark NS6 %s failed: 0x%08x\n",operation,result);return false;}
static IOReturn control(UInt8 type,UInt8 request,UInt16 value,UInt16 index,void *data,UInt16 length){
    IOUSBDevRequest r={type,request,value,index,length,data,0}; return (*interface)->ControlRequest(interface,0,&r);
}
static void fill(struct slot *slot){
    unsigned char *output=slot->data;
    if(++control_slots==CONTROL_SLOTS){
        int32_t desired=((int32_t)ns6_transport_available()-(int32_t)QUEUE_TARGET_FRAMES)*2;
        if(desired>MAX_RATE_ADJUST_Q16)desired=MAX_RATE_ADJUST_Q16;
        if(desired<-MAX_RATE_ADJUST_Q16)desired=-MAX_RATE_ADJUST_Q16;
        rate_adjust_q16+=(desired-rate_adjust_q16)/8;
        control_slots=0;
    }
    for(unsigned packet=0;packet<PACKETS;++packet){
        fraction+=(uint64_t)(((int64_t)RATE<<16)+rate_adjust_q16);
        unsigned frames=(unsigned)(fraction/((uint64_t)8000<<16)); fraction%=((uint64_t)8000<<16);
        slot->frames[packet].frReqCount=(UInt16)(frames*FRAME_BYTES); slot->frames[packet].frActCount=0; slot->frames[packet].frStatus=0;
        UInt32 received=ns6_transport_dequeue(output,frames);
        if(received<frames)memset(output+received*FRAME_BYTES,0,(frames-received)*FRAME_BYTES);
        output+=frames*FRAME_BYTES;
    }
}
static void submit(struct slot *slot);
static void complete(void *reference,IOReturn status,void *argument){(void)argument;if(status==kIOReturnSuccess&&atomic_load_explicit(&running,memory_order_acquire))submit(reference);}
static void submit(struct slot *slot){
    fill(slot); IOReturn result=(*interface)->LowLatencyWriteIsochPipeAsync(interface,1,slot->data,0,PACKETS,1,slot->frames,complete,slot);
    if(result!=kIOReturnSuccess)atomic_store_explicit(&running,false,memory_order_release);
}
static bool initialize(void){
    unsigned char capability[64]={0},rate[3]={0x44,0xac,0}; interface=open_interface(); if(!interface)return false;
    IOReturn result=(*interface)->USBInterfaceOpenSeize(interface); if(result!=kIOReturnSuccess)return usb_failure("open interface",result);
    auxiliary_interface=open_interface_once(1); if(!auxiliary_interface)return false;
    result=(*auxiliary_interface)->USBInterfaceOpenSeize(auxiliary_interface); if(result!=kIOReturnSuccess)return usb_failure("open auxiliary interface",result);
    result=(*auxiliary_interface)->SetAlternateInterface(auxiliary_interface,1); if(result!=kIOReturnSuccess)return usb_failure("select auxiliary alternate setting",result);
    result=(*interface)->CreateInterfaceAsyncEventSource(interface,&source); if(result!=kIOReturnSuccess||!source)return usb_failure("create async source",result);
    run_loop=CFRunLoopGetCurrent(); CFRetain(run_loop); CFRunLoopAddSource(run_loop,source,kCFRunLoopDefaultMode);
    result=(*interface)->SetAlternateInterface(interface,1); if(result!=kIOReturnSuccess)return usb_failure("select alternate setting",result);
    result=control(0xc0,86,0,0,capability,8); if(result!=kIOReturnSuccess)return usb_failure("read capability",result);
    UInt16 capability_length=capability[0]<sizeof(capability)?capability[0]:(UInt16)sizeof(capability);
    if(capability_length){result=control(0xc0,86,0,0,capability,capability_length);if(result!=kIOReturnSuccess)return usb_failure("read capability details",result);}
    result=control(0x22,1,0x0100,134,rate,sizeof(rate));if(result!=kIOReturnSuccess)return usb_failure("set clock selector 134",result);
    result=control(0x22,1,0x0100,2,rate,sizeof(rate));if(result!=kIOReturnSuccess)return usb_failure("set clock selector 2",result);
    result=control(0x40,73,0x0032,0,NULL,0);if(result!=kIOReturnSuccess)return usb_failure("start audio engine",result);
    result=activate_controller();if(result!=kIOReturnSuccess)return usb_failure("activate controller",result);
    for(unsigned i=0;i<SLOT_COUNT;++i){
        result=(*interface)->LowLatencyCreateBuffer(interface,(void**)&slots[i].data,PACKETS*MAX_PACKET,kUSBLowLatencyWriteBuffer);if(result!=kIOReturnSuccess)return usb_failure("create audio buffer",result);
        result=(*interface)->LowLatencyCreateBuffer(interface,(void**)&slots[i].frames,sizeof(*slots[i].frames)*PACKETS,kUSBLowLatencyFrameListBuffer);if(result!=kIOReturnSuccess)return usb_failure("create frame list",result);
    }
    return true;
}
static void cleanup(void){
    if(interface)(*interface)->AbortPipe(interface,1); if(run_loop)CFRunLoopRunInMode(kCFRunLoopDefaultMode,0.2,false);
    if(interface)for(unsigned i=0;i<SLOT_COUNT;++i){if(slots[i].data)(*interface)->LowLatencyDestroyBuffer(interface,slots[i].data);if(slots[i].frames)(*interface)->LowLatencyDestroyBuffer(interface,slots[i].frames);} memset(slots,0,sizeof(slots));
    if(run_loop&&source)CFRunLoopRemoveSource(run_loop,source,kCFRunLoopDefaultMode); if(source)CFRelease(source); source=NULL;
    if(run_loop)CFRelease(run_loop); run_loop=NULL;
    if(auxiliary_interface){(*auxiliary_interface)->USBInterfaceClose(auxiliary_interface);(*auxiliary_interface)->Release(auxiliary_interface);} auxiliary_interface=NULL;
    if(interface){(*interface)->USBInterfaceClose(interface);(*interface)->Release(interface);} interface=NULL;
}
static void *worker(void *unused){
    (void)unused; bool ready=initialize(); pthread_mutex_lock(&lock); initialized=ready; if(!ready)atomic_store(&running,false); pthread_cond_broadcast(&changed); pthread_mutex_unlock(&lock);
    if(ready){
        const struct timespec millisecond={0,1000000};
        for(unsigned waited=0;waited<STARTUP_WAIT_MS&&atomic_load_explicit(&running,memory_order_acquire)&&ns6_transport_available()<STARTUP_FRAMES;++waited)nanosleep(&millisecond,NULL);
        for(unsigned i=0;i<SLOT_COUNT;++i)submit(&slots[i]);
        while(atomic_load_explicit(&running,memory_order_acquire))CFRunLoopRunInMode(kCFRunLoopDefaultMode,0.1,false);
    } cleanup(); return NULL;
}
bool ns6_usb_start(void){
    pthread_mutex_lock(&lock); if(thread_created){bool result=initialized;pthread_mutex_unlock(&lock);return result;}
    fraction=0; rate_adjust_q16=0; control_slots=0; initialized=false; ns6_transport_reset(); atomic_store(&running,true);
    if(pthread_create(&thread,NULL,worker,NULL)!=0){atomic_store(&running,false);pthread_mutex_unlock(&lock);return false;}
    thread_created=true; while(!initialized&&atomic_load(&running))pthread_cond_wait(&changed,&lock); bool result=initialized; pthread_mutex_unlock(&lock);
    if(!result){pthread_join(thread,NULL);pthread_mutex_lock(&lock);thread_created=false;pthread_mutex_unlock(&lock);} return result;
}
void ns6_usb_stop(void){
    pthread_mutex_lock(&lock); if(!thread_created){pthread_mutex_unlock(&lock);return;} atomic_store(&running,false); if(run_loop)CFRunLoopWakeUp(run_loop); pthread_mutex_unlock(&lock);
    pthread_join(thread,NULL); pthread_mutex_lock(&lock); thread_created=false; initialized=false; pthread_mutex_unlock(&lock);
}
void ns6_usb_submit_pcm(const uint8_t *pcm24,uint32_t frames){(void)pcm24;(void)frames;}
