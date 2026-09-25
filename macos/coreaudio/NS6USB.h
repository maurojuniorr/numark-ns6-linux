#pragma once
#include <stdbool.h>
#include <stdint.h>

/* Runs outside CoreAudio's real-time thread. */
bool ns6_usb_start(void);
void ns6_usb_stop(void);
void ns6_usb_submit_pcm(const uint8_t *pcm24, uint32_t frames);
