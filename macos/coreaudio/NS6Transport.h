#pragma once
#include <CoreAudio/CoreAudioTypes.h>
#include <stdbool.h>
bool ns6_transport_enqueue(const void *frames, UInt32 count, const AudioStreamBasicDescription *format);
UInt32 ns6_transport_dequeue(void *frames, UInt32 capacity);
UInt32 ns6_transport_available(void);
void ns6_transport_reset(void);
