#include "NS6Transport.h"
#include "NS6USB.h"
#include <CoreAudio/AudioHardware.h>
#include <CoreAudio/AudioServerPlugIn.h>
#include <CoreFoundation/CFPlugInCOM.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/usb/IOUSBLib.h>
#include <mach/mach_time.h>
#include <pthread.h>
#include <stddef.h>
#include <stdatomic.h>
#include <string.h>

enum { DEVICE_ID=2, STREAM_ID=3 };
#define SAMPLE_RATE 44100.0
#define CHANNELS 4
#define BUFFER_FRAMES 512

static AudioServerPlugInHostRef host;
static UInt32 references=1,io_clients,seed=1;
static UInt64 anchor_host_time;
static mach_timebase_info_data_t timebase;
static IONotificationPortRef usb_notifications;
static io_iterator_t usb_added,usb_removed;
static atomic_bool device_present;
static pthread_once_t usb_monitor_once=PTHREAD_ONCE_INIT;
static AudioStreamBasicDescription stream_format={SAMPLE_RATE,kAudioFormatLinearPCM,kAudioFormatFlagIsFloat|kAudioFormatFlagsNativeEndian|kAudioFormatFlagIsPacked,16,1,16,CHANNELS,32,0};

static HRESULT query_interface(void*,REFIID,LPVOID*); static ULONG add_ref(void*); static ULONG release_ref(void*);
static OSStatus initialize(AudioServerPlugInDriverRef,AudioServerPlugInHostRef); static OSStatus create_device(AudioServerPlugInDriverRef,CFDictionaryRef,const AudioServerPlugInClientInfo*,AudioObjectID*); static OSStatus destroy_device(AudioServerPlugInDriverRef,AudioObjectID);
static OSStatus add_client(AudioServerPlugInDriverRef,AudioObjectID,const AudioServerPlugInClientInfo*); static OSStatus remove_client(AudioServerPlugInDriverRef,AudioObjectID,const AudioServerPlugInClientInfo*); static OSStatus perform_change(AudioServerPlugInDriverRef,AudioObjectID,UInt64,void*); static OSStatus abort_change(AudioServerPlugInDriverRef,AudioObjectID,UInt64,void*);
static Boolean has_property(AudioServerPlugInDriverRef,AudioObjectID,pid_t,const AudioObjectPropertyAddress*); static OSStatus is_settable(AudioServerPlugInDriverRef,AudioObjectID,pid_t,const AudioObjectPropertyAddress*,Boolean*); static OSStatus property_size(AudioServerPlugInDriverRef,AudioObjectID,pid_t,const AudioObjectPropertyAddress*,UInt32,const void*,UInt32*); static OSStatus get_property(AudioServerPlugInDriverRef,AudioObjectID,pid_t,const AudioObjectPropertyAddress*,UInt32,const void*,UInt32,UInt32*,void*); static OSStatus set_property(AudioServerPlugInDriverRef,AudioObjectID,pid_t,const AudioObjectPropertyAddress*,UInt32,const void*,UInt32,const void*);
static OSStatus start_io(AudioServerPlugInDriverRef,AudioObjectID,UInt32); static OSStatus stop_io(AudioServerPlugInDriverRef,AudioObjectID,UInt32); static OSStatus zero_timestamp(AudioServerPlugInDriverRef,AudioObjectID,UInt32,Float64*,UInt64*,UInt64*); static OSStatus will_do(AudioServerPlugInDriverRef,AudioObjectID,UInt32,UInt32,Boolean*,Boolean*); static OSStatus begin_io(AudioServerPlugInDriverRef,AudioObjectID,UInt32,UInt32,UInt32,const AudioServerPlugInIOCycleInfo*); static OSStatus do_io(AudioServerPlugInDriverRef,AudioObjectID,AudioObjectID,UInt32,UInt32,UInt32,const AudioServerPlugInIOCycleInfo*,void*,void*); static OSStatus end_io(AudioServerPlugInDriverRef,AudioObjectID,UInt32,UInt32,UInt32,const AudioServerPlugInIOCycleInfo*);

static AudioServerPlugInDriverInterface interface={NULL,query_interface,add_ref,release_ref,initialize,create_device,destroy_device,add_client,remove_client,perform_change,abort_change,has_property,is_settable,property_size,get_property,set_property,start_io,stop_io,zero_timestamp,will_do,begin_io,do_io,end_io};
static AudioServerPlugInDriverInterface *interface_pointer=&interface;
static AudioServerPlugInDriverRef driver=&interface_pointer;

static bool usb_property_u16(io_registry_entry_t entry,CFStringRef key,UInt16 *out){CFTypeRef value=IORegistryEntryCreateCFProperty(entry,key,kCFAllocatorDefault,0);bool ok=value&&CFGetTypeID(value)==CFNumberGetTypeID()&&CFNumberGetValue(value,kCFNumberSInt16Type,out);if(value)CFRelease(value);return ok;}
static bool ns6_connected(void){io_iterator_t iterator=IO_OBJECT_NULL;io_service_t service;if(IOServiceGetMatchingServices(kIOMainPortDefault,IOServiceMatching("IOUSBHostDevice"),&iterator)!=KERN_SUCCESS)return false;bool found=false;while((service=IOIteratorNext(iterator))){UInt16 vendor=0,product=0;if(usb_property_u16(service,CFSTR(kUSBVendorID),&vendor)&&usb_property_u16(service,CFSTR(kUSBProductID),&product)&&vendor==0x15e4&&product==0x0079)found=true;IOObjectRelease(service);if(found)break;}IOObjectRelease(iterator);return found;}
static void publish_connection_state(void){bool connected=ns6_connected();bool previous=atomic_exchange_explicit(&device_present,connected,memory_order_acq_rel);if(previous==connected||!host)return;AudioObjectPropertyAddress plugin_change={kAudioPlugInPropertyDeviceList,kAudioObjectPropertyScopeGlobal,kAudioObjectPropertyElementMain};host->PropertiesChanged(host,kAudioObjectPlugInObject,1,&plugin_change);AudioObjectPropertyAddress device_change={kAudioDevicePropertyDeviceIsAlive,kAudioObjectPropertyScopeGlobal,kAudioObjectPropertyElementMain};host->PropertiesChanged(host,DEVICE_ID,1,&device_change);}
static void usb_changed(void *reference,io_iterator_t iterator){(void)reference;io_service_t service;while((service=IOIteratorNext(iterator)))IOObjectRelease(service);publish_connection_state();}
static void *usb_monitor_thread(void *unused){(void)unused;usb_notifications=IONotificationPortCreate(kIOMainPortDefault);if(!usb_notifications)return NULL;CFRunLoopAddSource(CFRunLoopGetCurrent(),IONotificationPortGetRunLoopSource(usb_notifications),kCFRunLoopDefaultMode);if(IOServiceAddMatchingNotification(usb_notifications,kIOFirstMatchNotification,IOServiceMatching("IOUSBHostDevice"),usb_changed,NULL,&usb_added)==KERN_SUCCESS)usb_changed(NULL,usb_added);if(IOServiceAddMatchingNotification(usb_notifications,kIOTerminatedNotification,IOServiceMatching("IOUSBHostDevice"),usb_changed,NULL,&usb_removed)==KERN_SUCCESS)usb_changed(NULL,usb_removed);CFRunLoopRun();return NULL;}
static void monitor_usb(void){pthread_t thread;if(pthread_create(&thread,NULL,usb_monitor_thread,NULL)==0)pthread_detach(thread);}

void *NS6_Create(CFAllocatorRef allocator,CFUUIDRef type){(void)allocator;return CFEqual(type,kAudioServerPlugInTypeUUID)?driver:NULL;}
static bool valid_object(AudioObjectID object){return object==kAudioObjectPlugInObject||object==DEVICE_ID||object==STREAM_ID;}
static HRESULT query_interface(void *value,REFIID uuid,LPVOID *out){if(value!=driver||!out)return E_NOINTERFACE;CFUUIDRef requested=CFUUIDCreateFromUUIDBytes(NULL,uuid);HRESULT result=E_NOINTERFACE;if(requested&&(CFEqual(requested,IUnknownUUID)||CFEqual(requested,kAudioServerPlugInDriverInterfaceUUID))){*out=driver;++references;result=0;}if(requested)CFRelease(requested);return result;}
static ULONG add_ref(void *value){return value==driver?++references:0;} static ULONG release_ref(void *value){return value==driver&&references?--references:0;}
static OSStatus initialize(AudioServerPlugInDriverRef value,AudioServerPlugInHostRef value_host){if(value!=driver)return kAudioHardwareBadObjectError;host=value_host;mach_timebase_info(&timebase);atomic_store(&device_present,ns6_connected());pthread_once(&usb_monitor_once,monitor_usb);return 0;}
static OSStatus create_device(AudioServerPlugInDriverRef d,CFDictionaryRef x,const AudioServerPlugInClientInfo*y,AudioObjectID*z){(void)d;(void)x;(void)y;(void)z;return kAudioHardwareUnsupportedOperationError;} static OSStatus destroy_device(AudioServerPlugInDriverRef d,AudioObjectID x){(void)d;(void)x;return kAudioHardwareUnsupportedOperationError;}
static OSStatus add_client(AudioServerPlugInDriverRef d,AudioObjectID x,const AudioServerPlugInClientInfo*y){(void)d;(void)x;(void)y;return 0;} static OSStatus remove_client(AudioServerPlugInDriverRef d,AudioObjectID x,const AudioServerPlugInClientInfo*y){(void)d;(void)x;(void)y;return 0;} static OSStatus perform_change(AudioServerPlugInDriverRef d,AudioObjectID x,UInt64 y,void*z){(void)d;(void)x;(void)y;(void)z;return 0;} static OSStatus abort_change(AudioServerPlugInDriverRef d,AudioObjectID x,UInt64 y,void*z){(void)d;(void)x;(void)y;(void)z;return 0;}

static Boolean has_property(AudioServerPlugInDriverRef d,AudioObjectID object,pid_t pid,const AudioObjectPropertyAddress *address){
    (void)d;(void)pid;if(!valid_object(object)||!address)return false;
    switch(address->mSelector){case kAudioObjectPropertyBaseClass:case kAudioObjectPropertyClass:case kAudioObjectPropertyOwner:case kAudioObjectPropertyName:return true;default:break;}
    if(object==kAudioObjectPlugInObject)switch(address->mSelector){case kAudioObjectPropertyManufacturer:case kAudioObjectPropertyOwnedObjects:case kAudioPlugInPropertyDeviceList:case kAudioPlugInPropertyResourceBundle:return true;default:return false;}
    if(object==DEVICE_ID)switch(address->mSelector){case kAudioObjectPropertyManufacturer:case kAudioObjectPropertyOwnedObjects:case kAudioDevicePropertyDeviceUID:case kAudioDevicePropertyModelUID:case kAudioDevicePropertyTransportType:case kAudioDevicePropertyRelatedDevices:case kAudioDevicePropertyClockDomain:case kAudioDevicePropertyDeviceIsAlive:case kAudioDevicePropertyDeviceIsRunning:case kAudioDevicePropertyDeviceCanBeDefaultDevice:case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice:case kAudioDevicePropertyLatency:case kAudioDevicePropertyStreams:case kAudioObjectPropertyControlList:case kAudioDevicePropertySafetyOffset:case kAudioDevicePropertyNominalSampleRate:case kAudioDevicePropertyAvailableNominalSampleRates:case kAudioDevicePropertyIsHidden:case kAudioDevicePropertyPreferredChannelsForStereo:case kAudioDevicePropertyBufferFrameSize:case kAudioDevicePropertyBufferFrameSizeRange:case kAudioDevicePropertyStreamConfiguration:case kAudioDevicePropertyZeroTimeStampPeriod:return true;default:return false;}
    switch(address->mSelector){case kAudioStreamPropertyIsActive:case kAudioStreamPropertyDirection:case kAudioStreamPropertyTerminalType:case kAudioStreamPropertyStartingChannel:case kAudioStreamPropertyLatency:case kAudioStreamPropertyVirtualFormat:case kAudioStreamPropertyPhysicalFormat:case kAudioStreamPropertyAvailableVirtualFormats:case kAudioStreamPropertyAvailablePhysicalFormats:return true;default:return false;}
}
static OSStatus is_settable(AudioServerPlugInDriverRef d,AudioObjectID o,pid_t p,const AudioObjectPropertyAddress*a,Boolean*out){if(!out)return kAudioHardwareIllegalOperationError;if(!has_property(d,o,p,a))return kAudioHardwareUnknownPropertyError;*out=false;return 0;}
static UInt32 scoped_count(const AudioObjectPropertyAddress *a){return a->mScope==kAudioObjectPropertyScopeInput?0:1;}
static OSStatus property_size(AudioServerPlugInDriverRef d,AudioObjectID object,pid_t pid,const AudioObjectPropertyAddress *a,UInt32 q,const void*x,UInt32 *out){
    (void)q;(void)x;if(!out)return kAudioHardwareIllegalOperationError;if(!has_property(d,object,pid,a))return kAudioHardwareUnknownPropertyError;
    switch(a->mSelector){
        case kAudioObjectPropertyName:case kAudioObjectPropertyManufacturer:case kAudioDevicePropertyDeviceUID:case kAudioDevicePropertyModelUID:case kAudioPlugInPropertyResourceBundle:*out=sizeof(CFStringRef);break;
        case kAudioObjectPropertyOwnedObjects:*out=(object==kAudioObjectPlugInObject&&!atomic_load(&device_present))?0:((object==kAudioObjectPlugInObject||object==DEVICE_ID)?sizeof(AudioObjectID):0);break;
        case kAudioPlugInPropertyDeviceList:*out=atomic_load(&device_present)?sizeof(AudioObjectID):0;break;
        case kAudioDevicePropertyRelatedDevices:*out=sizeof(AudioObjectID);break;
        case kAudioDevicePropertyStreams:*out=scoped_count(a)*sizeof(AudioObjectID);break;
        case kAudioObjectPropertyControlList:*out=0;break;
        case kAudioDevicePropertyNominalSampleRate:*out=sizeof(Float64);break;
        case kAudioDevicePropertyAvailableNominalSampleRates:case kAudioDevicePropertyBufferFrameSizeRange:*out=sizeof(AudioValueRange);break;
        case kAudioDevicePropertyPreferredChannelsForStereo:*out=2*sizeof(UInt32);break;
        case kAudioDevicePropertyStreamConfiguration:*out=offsetof(AudioBufferList,mBuffers)+sizeof(AudioBuffer);break;
        case kAudioStreamPropertyVirtualFormat:case kAudioStreamPropertyPhysicalFormat:*out=sizeof(AudioStreamBasicDescription);break;
        case kAudioStreamPropertyAvailableVirtualFormats:case kAudioStreamPropertyAvailablePhysicalFormats:*out=sizeof(AudioStreamRangedDescription);break;
        default:*out=sizeof(UInt32);break;
    }return 0;
}
static CFStringRef string_property(AudioObjectID object,AudioObjectPropertySelector selector){
    if(selector==kAudioObjectPropertyManufacturer)return CFSTR("Numark / community driver");
    if(selector==kAudioDevicePropertyDeviceUID)return CFSTR("io.github.maurojuniorr.numark-ns6.device");
    if(selector==kAudioDevicePropertyModelUID)return CFSTR("io.github.maurojuniorr.numark-ns6.original");
    if(selector==kAudioPlugInPropertyResourceBundle)return CFSTR("");
    if(object==kAudioObjectPlugInObject)return CFSTR("Numark NS6 Plug-In");
    if(object==DEVICE_ID)return CFSTR("Numark NS6"); return CFSTR("Numark NS6 Output");
}
static OSStatus get_property(AudioServerPlugInDriverRef d,AudioObjectID object,pid_t pid,const AudioObjectPropertyAddress *a,UInt32 q,const void*x,UInt32 input_size,UInt32 *output_size,void *output){
    UInt32 needed=0;OSStatus status=property_size(d,object,pid,a,q,x,&needed);if(status)return status;if(input_size<needed)return kAudioHardwareBadPropertySizeError;if(output_size)*output_size=needed;if(!needed)return 0;if(!output)return kAudioHardwareIllegalOperationError;
    switch(a->mSelector){
        case kAudioObjectPropertyName:case kAudioObjectPropertyManufacturer:case kAudioDevicePropertyDeviceUID:case kAudioDevicePropertyModelUID:case kAudioPlugInPropertyResourceBundle:*(CFStringRef*)output=CFRetain(string_property(object,a->mSelector));return 0;
        case kAudioObjectPropertyBaseClass:*(AudioClassID*)output=kAudioObjectClassID;return 0;
        case kAudioObjectPropertyClass:*(AudioClassID*)output=object==kAudioObjectPlugInObject?kAudioPlugInClassID:(object==DEVICE_ID?kAudioDeviceClassID:kAudioStreamClassID);return 0;
        case kAudioObjectPropertyOwner:*(AudioObjectID*)output=object==STREAM_ID?DEVICE_ID:kAudioObjectUnknown;return 0;
        case kAudioObjectPropertyOwnedObjects:*(AudioObjectID*)output=object==kAudioObjectPlugInObject?DEVICE_ID:STREAM_ID;return 0;
        case kAudioPlugInPropertyDeviceList:if(atomic_load(&device_present))*(AudioObjectID*)output=DEVICE_ID;return 0;
        case kAudioDevicePropertyRelatedDevices:*(AudioObjectID*)output=DEVICE_ID;return 0;
        case kAudioDevicePropertyStreams:*(AudioObjectID*)output=STREAM_ID;return 0;
        case kAudioDevicePropertyNominalSampleRate:*(Float64*)output=SAMPLE_RATE;return 0;
        case kAudioDevicePropertyAvailableNominalSampleRates:{AudioValueRange r={SAMPLE_RATE,SAMPLE_RATE};*(AudioValueRange*)output=r;return 0;}
        case kAudioDevicePropertyBufferFrameSizeRange:{AudioValueRange r={64,2048};*(AudioValueRange*)output=r;return 0;}
        case kAudioDevicePropertyPreferredChannelsForStereo:((UInt32*)output)[0]=1;((UInt32*)output)[1]=2;return 0;
        case kAudioDevicePropertyStreamConfiguration:{AudioBufferList *list=output;list->mNumberBuffers=1;list->mBuffers[0].mNumberChannels=scoped_count(a)?CHANNELS:0;list->mBuffers[0].mDataByteSize=0;list->mBuffers[0].mData=NULL;return 0;}
        case kAudioStreamPropertyVirtualFormat:case kAudioStreamPropertyPhysicalFormat:*(AudioStreamBasicDescription*)output=stream_format;return 0;
        case kAudioStreamPropertyAvailableVirtualFormats:case kAudioStreamPropertyAvailablePhysicalFormats:{AudioStreamRangedDescription r={stream_format,{SAMPLE_RATE,SAMPLE_RATE}};*(AudioStreamRangedDescription*)output=r;return 0;}
        default:break;
    }
    UInt32 value=0;
    switch(a->mSelector){case kAudioDevicePropertyTransportType:value=kAudioDeviceTransportTypeUSB;break;case kAudioDevicePropertyDeviceIsAlive:value=atomic_load(&device_present)?1:0;break;case kAudioDevicePropertyDeviceCanBeDefaultDevice:case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice:case kAudioStreamPropertyIsActive:value=1;break;case kAudioDevicePropertyDeviceIsRunning:value=io_clients?1:0;break;case kAudioDevicePropertyBufferFrameSize:value=BUFFER_FRAMES;break;case kAudioDevicePropertyZeroTimeStampPeriod:value=BUFFER_FRAMES;break;case kAudioStreamPropertyDirection:value=0;break;case kAudioStreamPropertyTerminalType:value=kAudioStreamTerminalTypeLine;break;case kAudioStreamPropertyStartingChannel:value=1;break;default:value=0;break;}*(UInt32*)output=value;return 0;
}
static OSStatus set_property(AudioServerPlugInDriverRef d,AudioObjectID o,pid_t p,const AudioObjectPropertyAddress*a,UInt32 q,const void*x,UInt32 n,const void*v){(void)d;(void)o;(void)p;(void)a;(void)q;(void)x;(void)n;(void)v;return kAudioHardwareUnsupportedOperationError;}

static OSStatus start_io(AudioServerPlugInDriverRef d,AudioObjectID object,UInt32 client){(void)d;(void)client;if(object!=DEVICE_ID)return kAudioHardwareBadObjectError;if(!atomic_load(&device_present))return kAudioHardwareNotRunningError;if(io_clients==0){ns6_transport_reset();if(!ns6_usb_start())return kAudioHardwareUnspecifiedError;anchor_host_time=mach_absolute_time();++seed;}++io_clients;return 0;}
static OSStatus stop_io(AudioServerPlugInDriverRef d,AudioObjectID object,UInt32 client){(void)d;(void)client;if(object!=DEVICE_ID)return kAudioHardwareBadObjectError;if(io_clients&&--io_clients==0)ns6_usb_stop();return 0;}
static OSStatus zero_timestamp(AudioServerPlugInDriverRef d,AudioObjectID object,UInt32 client,Float64 *sample,UInt64 *host_time,UInt64 *out_seed){(void)d;(void)client;if(object!=DEVICE_ID||!sample||!host_time||!out_seed)return kAudioHardwareIllegalOperationError;UInt64 now=mach_absolute_time();double nanos=(double)(now-anchor_host_time)*timebase.numer/timebase.denom;UInt64 frame=(UInt64)(nanos*SAMPLE_RATE/1e9);frame=(frame/BUFFER_FRAMES)*BUFFER_FRAMES;double ticks=(double)frame*1e9/SAMPLE_RATE*timebase.denom/timebase.numer;*sample=(Float64)frame;*host_time=anchor_host_time+(UInt64)ticks;*out_seed=seed;return 0;}
static OSStatus will_do(AudioServerPlugInDriverRef d,AudioObjectID object,UInt32 client,UInt32 operation,Boolean *will,Boolean *in_place){(void)d;(void)object;(void)client;if(!will||!in_place)return kAudioHardwareIllegalOperationError;*will=operation==kAudioServerPlugInIOOperationWriteMix;*in_place=true;return 0;}
static OSStatus begin_io(AudioServerPlugInDriverRef d,AudioObjectID o,UInt32 c,UInt32 op,UInt32 frames,const AudioServerPlugInIOCycleInfo*i){(void)d;(void)o;(void)c;(void)op;(void)frames;(void)i;return 0;}
static OSStatus do_io(AudioServerPlugInDriverRef d,AudioObjectID object,AudioObjectID stream,UInt32 client,UInt32 operation,UInt32 frames,const AudioServerPlugInIOCycleInfo*i,void *main_buffer,void *secondary){(void)d;(void)client;(void)i;(void)secondary;if(object!=DEVICE_ID||stream!=STREAM_ID)return kAudioHardwareBadObjectError;if(operation==kAudioServerPlugInIOOperationWriteMix&&!ns6_transport_enqueue(main_buffer,frames,&stream_format))return kAudioHardwareUnspecifiedError;return 0;}
static OSStatus end_io(AudioServerPlugInDriverRef d,AudioObjectID o,UInt32 c,UInt32 op,UInt32 frames,const AudioServerPlugInIOCycleInfo*i){(void)d;(void)o;(void)c;(void)op;(void)frames;(void)i;return 0;}
