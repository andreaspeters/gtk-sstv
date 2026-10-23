#include "txaudio.h"

#include <alsa/asoundlib.h>
#include <errno.h>

#define TX_SAMPLE_RATE 44100
#define TX_CHUNK_FRAMES 2048

static void set_audio_error(GError **error, int code, const char *message) {
  if (error != NULL)
    g_set_error_literal(error, g_quark_from_static_string("slowrx-alsa-tx"),
                        code, message);
}

gboolean sstv_play_pcm(const char *device, const int16_t *samples,
                       size_t frames, GError **error) {
  snd_pcm_t *handle = NULL;
  size_t offset = 0;
  int status;

  if (device == NULL || *device == '\0' || samples == NULL || frames == 0) {
    set_audio_error(error, 1, "Playback requires a device and non-empty PCM data");
    return FALSE;
  }

  status = snd_pcm_open(&handle, device, SND_PCM_STREAM_PLAYBACK, 0);
  if (status < 0) {
    if (error != NULL)
      g_set_error(error, g_quark_from_static_string("slowrx-alsa-tx"), status,
                  "Cannot open ALSA playback device '%s': %s", device,
                  snd_strerror(status));
    return FALSE;
  }

  status = snd_pcm_set_params(handle, SND_PCM_FORMAT_S16_LE,
                              SND_PCM_ACCESS_RW_INTERLEAVED, 1,
                              TX_SAMPLE_RATE, 1, 100000);
  if (status < 0) {
    if (error != NULL)
      g_set_error(error, g_quark_from_static_string("slowrx-alsa-tx"), status,
                  "Cannot configure ALSA playback device '%s': %s", device,
                  snd_strerror(status));
    snd_pcm_close(handle);
    return FALSE;
  }

  while (offset < frames) {
    size_t remaining = frames - offset;
    snd_pcm_uframes_t chunk = (remaining < TX_CHUNK_FRAMES)
                                ? (snd_pcm_uframes_t)remaining
                                : TX_CHUNK_FRAMES;
    snd_pcm_sframes_t written = snd_pcm_writei(handle, samples + offset, chunk);
    if (written < 0) {
      status = snd_pcm_recover(handle, (int)written, 1);
      if (status < 0) {
        if (error != NULL)
          g_set_error(error, g_quark_from_static_string("slowrx-alsa-tx"), status,
                      "ALSA playback failed: %s", snd_strerror(status));
        snd_pcm_close(handle);
        return FALSE;
      }
      continue;
    }
    if (written == 0) continue;
    offset += (size_t)written;
  }

  status = snd_pcm_drain(handle);
  if (status < 0 && error != NULL)
    g_set_error(error, g_quark_from_static_string("slowrx-alsa-tx"), status,
                "ALSA playback drain failed: %s", snd_strerror(status));
  snd_pcm_close(handle);
  return status >= 0;
}
