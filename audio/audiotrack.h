/* FMOD's org/fmod/AudioDevice (Android AudioTrack output) on sceAudioOut. See audiotrack.c. */
#ifndef BV_AUDIOTRACK_H
#define BV_AUDIOTRACK_H
#include <stdint.h>
#define BV_AT_GRAIN 512 /* frames per sceAudioOutOutput call (a multiple of 64): 10.7 ms at 48 kHz */
int bv_audiotrack_init(int a0, int a1, int a2, int a3); /* AudioDevice.init(IIII)Z: 1 = port open */
void bv_audiotrack_write(const uint8_t *data, int len); /* AudioDevice.write([BI)V: len bytes of 16-bit PCM */
void bv_audiotrack_close(void);                         /* AudioDevice.close()V */
int bv_audiotrack_port(void);                           /* -1 when closed */
#endif
