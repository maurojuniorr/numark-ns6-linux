#include <CoreAudio/CoreAudio.h>
#include <CoreFoundation/CoreFoundation.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static CFStringRef device_name(AudioDeviceID device){CFStringRef name=NULL;UInt32 size=sizeof(name);AudioObjectPropertyAddress a={kAudioObjectPropertyName,kAudioObjectPropertyScopeGlobal,kAudioObjectPropertyElementMain};if(AudioObjectGetPropertyData(device,&a,0,NULL,&size,&name))return NULL;return name;}
int main(int argc,char **argv){
 AudioObjectPropertyAddress def={kAudioHardwarePropertyDefaultOutputDevice,kAudioObjectPropertyScopeGlobal,kAudioObjectPropertyElementMain};AudioDeviceID current=0;UInt32 size=sizeof(current);if(AudioObjectGetPropertyData(kAudioObjectSystemObject,&def,0,NULL,&size,&current))return 1;
 if(argc==1){CFStringRef name=device_name(current);char text[256]="";if(name){CFStringGetCString(name,text,sizeof(text),kCFStringEncodingUTF8);CFRelease(name);}printf("%u %s\n",current,text);return 0;}
 AudioObjectPropertyAddress list={kAudioHardwarePropertyDevices,kAudioObjectPropertyScopeGlobal,kAudioObjectPropertyElementMain};UInt32 bytes=0;if(AudioObjectGetPropertyDataSize(kAudioObjectSystemObject,&list,0,NULL,&bytes))return 1;AudioDeviceID *devices=malloc(bytes);if(AudioObjectGetPropertyData(kAudioObjectSystemObject,&list,0,NULL,&bytes,devices))return 1;AudioDeviceID selected=0;
 for(UInt32 i=0;i<bytes/sizeof(*devices);++i){CFStringRef name=device_name(devices[i]);char text[256]="";if(name){CFStringGetCString(name,text,sizeof(text),kCFStringEncodingUTF8);CFRelease(name);}if(!strcmp(text,argv[1])){selected=devices[i];break;}}
 free(devices);if(!selected){fprintf(stderr,"device not found: %s\n",argv[1]);return 1;}size=sizeof(selected);OSStatus status=AudioObjectSetPropertyData(kAudioObjectSystemObject,&def,0,NULL,size,&selected);if(status){fprintf(stderr,"set default: %d\n",status);return 1;}printf("%u\n",selected);return 0;
}
