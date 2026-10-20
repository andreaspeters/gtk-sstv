#ifndef SLOWRX_TXAUDIO_H
#define SLOWRX_TXAUDIO_H

#include <stddef.h>
#include <stdint.h>
#include <glib.h>

/* Blocking ALSA playback. Use an explicit device; automated tests use "null". */
gboolean sstv_play_pcm(const char *device, const int16_t *samples,
                       size_t frames, GError **error);

#endif
