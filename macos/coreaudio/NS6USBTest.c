#include "NS6Transport.h"
#include "NS6USB.h"
#include <CoreAudio/CoreAudioTypes.h>
#include <math.h>
#include <mach/mach_time.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define RATE 44100
#define CHANNELS 4
#define CHUNK 441

int main(int argc,char **argv){
    int seconds=argc>1?atoi(argv[1]):10; uint64_t frame=0; float pcm[CHUNK*CHANNELS];
    AudioStreamBasicDescription format={RATE,kAudioFormatLinearPCM,kAudioFormatFlagIsFloat|kAudioFormatFlagsNativeEndian|kAudioFormatFlagIsPacked,16,1,16,4,32,0};
    if(!ns6_usb_start()){fputs("NS6 USB worker failed to start\n",stderr);return 1;}
    mach_timebase_info_data_t timebase; mach_timebase_info(&timebase); uint64_t start=mach_absolute_time();
    for(int block=0;block<seconds*100;++block){
        for(int f=0;f<CHUNK;++f,++frame){float sample=(float)(sin(2.0*M_PI*440.0*frame/RATE)*0.1875);for(int c=0;c<CHANNELS;++c)pcm[f*CHANNELS+c]=sample;}
        while(!ns6_transport_enqueue(pcm,CHUNK,&format)){}
        uint64_t deadline=start+(uint64_t)(((uint64_t)(block+1)*CHUNK*1000000000ULL*timebase.denom)/(RATE*timebase.numer));
        mach_wait_until(deadline);
    }
    ns6_usb_stop(); puts("NS6 USB worker completed"); return 0;
}
