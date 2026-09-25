#include <CoreAudio/CoreAudio.h>
#include <CoreFoundation/CoreFoundation.h>
#include <stdio.h>
#include <stdlib.h>

int main(void){
    AudioObjectPropertyAddress address={kAudioHardwarePropertyDevices,kAudioObjectPropertyScopeGlobal,kAudioObjectPropertyElementMain};
    UInt32 bytes=0;OSStatus status=AudioObjectGetPropertyDataSize(kAudioObjectSystemObject,&address,0,NULL,&bytes);
    if(status){fprintf(stderr,"device list size: %d\n",status);return 1;}
    AudioDeviceID *devices=malloc(bytes);status=AudioObjectGetPropertyData(kAudioObjectSystemObject,&address,0,NULL,&bytes,devices);
    if(status){fprintf(stderr,"device list: %d\n",status);return 1;}
    for(UInt32 i=0;i<bytes/sizeof(*devices);++i){
        CFStringRef name=NULL;UInt32 size=sizeof(name);AudioObjectPropertyAddress n={kAudioObjectPropertyName,kAudioObjectPropertyScopeGlobal,kAudioObjectPropertyElementMain};
        if(!AudioObjectGetPropertyData(devices[i],&n,0,NULL,&size,&name)&&name){char text[256];if(CFStringGetCString(name,text,sizeof(text),kCFStringEncodingUTF8))printf("%u %s\n",devices[i],text);CFRelease(name);}
    }
    free(devices);return 0;
}
