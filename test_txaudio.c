#include "txaudio.h"

#include <stdio.h>
#include <stdlib.h>

int main(void) {
  int16_t samples[4410] = { 0 };
  GError *error = NULL;

  if (!sstv_play_pcm("null", samples, 4410, &error)) {
    fprintf(stderr, "ALSA null playback failed: %s\n",
            error != NULL ? error->message : "unknown error");
    if (error != NULL) g_error_free(error);
    return EXIT_FAILURE;
  }
  puts("ALSA null playback OK: 4410 mono S16 frames at 44.1 kHz");
  return EXIT_SUCCESS;
}
