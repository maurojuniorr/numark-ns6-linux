#include "NS6Transport.h"
#include <math.h>
#include <stdatomic.h>
#include <stdint.h>
#include <string.h>

#define QUEUE_FRAMES 16384
#define CHANNELS 4
#define USB_FRAME_BYTES 12
static unsigned char queue[QUEUE_FRAMES*USB_FRAME_BYTES];
static _Atomic uint32_t write_position,read_position;

static void pack24(unsigned char *out,int32_t sample){out[0]=(unsigned char)sample;out[1]=(unsigned char)(sample>>8);out[2]=(unsigned char)(sample>>16);}
void ns6_transport_reset(void){atomic_store(&read_position,0);atomic_store(&write_position,0);}
UInt32 ns6_transport_available(void){
    uint32_t read=atomic_load_explicit(&read_position,memory_order_acquire);
    uint32_t write=atomic_load_explicit(&write_position,memory_order_acquire);
    return write-read;
}
bool ns6_transport_enqueue(const void *input,UInt32 count,const AudioStreamBasicDescription *format){
    if(!input||!format||format->mChannelsPerFrame!=CHANNELS||!format->mBytesPerFrame)return false;
    uint32_t write=atomic_load_explicit(&write_position,memory_order_relaxed),read=atomic_load_explicit(&read_position,memory_order_acquire);
    if(count>QUEUE_FRAMES-(write-read))return false;
    const unsigned char *source=input; bool is_float=(format->mFormatFlags&kAudioFormatFlagIsFloat)!=0; unsigned sample_bytes=format->mBytesPerFrame/CHANNELS;
    for(UInt32 frame=0;frame<count;++frame,source+=format->mBytesPerFrame){
        unsigned char *destination=queue+((write++%QUEUE_FRAMES)*USB_FRAME_BYTES);
        int32_t samples[CHANNELS];
        for(unsigned channel=0;channel<CHANNELS;++channel){
            const unsigned char *sample_source=source+channel*sample_bytes; int32_t sample;
            if(is_float&&sample_bytes==sizeof(float)){float value;memcpy(&value,sample_source,sizeof(value));if(value>1.0f)value=1.0f;if(value<-1.0f)value=-1.0f;sample=(int32_t)lrintf(value*8388607.0f);}
            else if(format->mBitsPerChannel==24&&sample_bytes==3)sample=(int32_t)sample_source[0]|((int32_t)sample_source[1]<<8)|((int32_t)(int8_t)sample_source[2]<<16);
            else if(format->mBitsPerChannel==32&&sample_bytes==4){int32_t value;memcpy(&value,sample_source,sizeof(value));sample=value>>8;}
            else return false;
            samples[channel]=sample;
        }
        for(unsigned channel=0;channel<CHANNELS;++channel)pack24(destination+channel*3,samples[channel]);
    }
    atomic_store_explicit(&write_position,write,memory_order_release);return true;
}
UInt32 ns6_transport_dequeue(void *output,UInt32 capacity){
    uint32_t read=atomic_load_explicit(&read_position,memory_order_relaxed),write=atomic_load_explicit(&write_position,memory_order_acquire);
    UInt32 count=(write-read)<capacity?(write-read):capacity;unsigned char *destination=output;
    for(UInt32 frame=0;frame<count;++frame,++read)memcpy(destination+frame*USB_FRAME_BYTES,queue+((read%QUEUE_FRAMES)*USB_FRAME_BYTES),USB_FRAME_BYTES);
    atomic_store_explicit(&read_position,read,memory_order_release);return count;
}
