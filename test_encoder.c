#include "encoder.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#define SAMPLE_RATE 44100

static int approx_frequency(const gint16 *samples, size_t start, size_t length) {
  size_t i;
  int rising = 0;
  for (i = start + 1; i < start + length; ++i) {
    if (samples[i - 1] <= 0 && samples[i] > 0) rising++;
  }
  return (int)lround((double)rising * SAMPLE_RATE / length);
}

static int check_martin_mode(GdkPixbuf *image, SstvMode mode,
                             unsigned int vis, double line_seconds) {
  SstvPcm pcm = { NULL, 0 };
  GError *error = NULL;
  size_t line_start = (size_t)llround(0.91 * SAMPLE_RATE);
  size_t scan_start = line_start + (size_t)llround((4.862 + 0.572) *
                                                   SAMPLE_RATE / 1000.0);
  size_t scan_ms = (mode == SSTV_MODE_MARTIN_M1) ? 146432 : 73216;
  size_t scan_samples = (size_t)llround(0.05 * SAMPLE_RATE);
  size_t blue_start = scan_start + (size_t)llround(scan_ms * SAMPLE_RATE / 1000000.0) +
                      (size_t)llround(0.572 * SAMPLE_RATE / 1000.0);
  size_t red_start = blue_start + (size_t)llround(scan_ms * SAMPLE_RATE / 1000000.0) +
                     (size_t)llround(0.572 * SAMPLE_RATE / 1000.0);
  size_t total = (size_t)llround((0.91 + 256.0 * line_seconds) * SAMPLE_RATE);
  int channel_hz[3] = { 1902, 2102, 1701 };
  size_t starts[3] = { scan_start, blue_start, red_start };
  int bit;

  if (!sstv_encode_mode(image, mode, &pcm, &error)) {
    fprintf(stderr, "%s encoding failed: %s\n", sstv_mode_name(mode),
            error != NULL ? error->message : "no error detail");
    if (error != NULL) g_error_free(error);
    return 0;
  }
  if (pcm.length != total || approx_frequency(pcm.samples, line_start, 180) < 1170 ||
      approx_frequency(pcm.samples, line_start, 180) > 1230) {
    fprintf(stderr, "%s output length or sync is incorrect (%zu samples)\n",
            sstv_mode_name(mode), pcm.length);
    free(pcm.samples);
    return 0;
  }

  for (bit = 0; bit < 7; ++bit) {
    size_t start = (size_t)llround((0.64 + bit * 0.03 + 0.015) * SAMPLE_RATE);
    int expected = ((vis >> bit) & 1) ? 1300 : 1100;
    int measured = approx_frequency(pcm.samples, start, 600);
    if (measured < expected - 30 || measured > expected + 30) {
      fprintf(stderr, "%s VIS bit %d incorrect (%d Hz)\n",
              sstv_mode_name(mode), bit, measured);
      free(pcm.samples);
      return 0;
    }
  }
  for (bit = 0; bit < 3; ++bit) {
    size_t start = starts[bit] + (size_t)llround(0.02 * SAMPLE_RATE);
    int measured = approx_frequency(pcm.samples, start, scan_samples);
    if (measured < channel_hz[bit] - 35 || measured > channel_hz[bit] + 35) {
      fprintf(stderr, "%s channel %d tone incorrect (%d Hz)\n",
              sstv_mode_name(mode), bit, measured);
      free(pcm.samples);
      return 0;
    }
  }
  free(pcm.samples);
  return 1;
}

static int check_fskid(void) {
  static const int expected_header_hz[18] = {
    2100, 2100, 2100, 2100, 2100, 1900,
    2100, 1900, 2100, 1900, 2100, 1900,
    2100, 1900, 1900, 1900, 2100, 1900
  };
  const char *callsign = "n0call";
  SstvPcm pcm = { NULL, 0 };
  SstvPcm invalid = { NULL, 0 };
  GError *error = NULL;
  size_t expected_length;
  size_t bit_index;

  expected_length = (size_t)llround((2 + strlen(callsign) + 1) * 6 *
                                    0.022 * SAMPLE_RATE);
  if (!sstv_encode_fskid(callsign, &pcm, &error)) {
    fprintf(stderr, "FSKID encoding failed: %s\n",
            error != NULL ? error->message : "no error detail");
    if (error != NULL) g_error_free(error);
    return 0;
  }
  if (pcm.samples == NULL || pcm.length != expected_length) {
    fprintf(stderr, "FSKID length incorrect (%zu samples, expected %zu)\n",
            pcm.length, expected_length);
    free(pcm.samples);
    return 0;
  }

  for (bit_index = 0; bit_index < 18; ++bit_index) {
    size_t start = (size_t)llround(bit_index * 0.022 * SAMPLE_RATE) + 80;
    int measured = approx_frequency(pcm.samples, start, 800);
    if (measured < expected_header_hz[bit_index] - 65 ||
        measured > expected_header_hz[bit_index] + 65) {
      fprintf(stderr, "FSKID header/callsign bit %zu incorrect (%d Hz)\n",
              bit_index, measured);
      free(pcm.samples);
      return 0;
    }
  }
  for (bit_index = 0; bit_index < 6; ++bit_index) {
    size_t symbol = (2 + strlen(callsign)) * 6 + bit_index;
    size_t start = (size_t)llround(symbol * 0.022 * SAMPLE_RATE) + 80;
    int expected_hz = bit_index == 0 ? 1900 : 2100;
    int measured = approx_frequency(pcm.samples, start, 800);
    if (measured < expected_hz - 65 || measured > expected_hz + 65) {
      fprintf(stderr, "FSKID terminator bit %zu incorrect (%d Hz)\n",
              bit_index, measured);
      free(pcm.samples);
      return 0;
    }
  }
  free(pcm.samples);

  if (sstv_encode_fskid("ABCDEFGHIJK", &invalid, &error) || error == NULL) {
    fprintf(stderr, "FSKID callsigns longer than 10 characters must be rejected\n");
    if (error != NULL) g_error_free(error);
    free(invalid.samples);
    return 0;
  }
  g_error_free(error);
  return 1;
}

int main(void) {
  GdkPixbuf *image;
  SstvPcm pcm = { NULL, 0 };
  GError *error = NULL;
  size_t first_sync;
  size_t vis_start;
  int leader_hz;
  int sync_hz;

  if (sstv_encode_robot36(NULL, &pcm, &error) || error == NULL) {
    fprintf(stderr, "null input must be rejected with an error\n");
    return EXIT_FAILURE;
  }
  g_error_free(error);
  error = NULL;

  image = gdk_pixbuf_new(GDK_COLORSPACE_RGB, FALSE, 8, 2, 2);
  if (image == NULL) return EXIT_FAILURE;
  gdk_pixbuf_fill(image, 0x4080c0ff);

  if (!sstv_encode_robot36(image, &pcm, &error)) {
    fprintf(stderr, "encoder failed: %s\n", error ? error->message : "no error detail");
    if (error != NULL) g_error_free(error);
    g_object_unref(image);
    return EXIT_FAILURE;
  }
  g_object_unref(image);

  if (pcm.samples == NULL || pcm.length != (size_t)llround(36.91 * SAMPLE_RATE)) {
    fprintf(stderr, "unexpected Robot 36 output size: %zu samples\n", pcm.length);
    free(pcm.samples);
    return EXIT_FAILURE;
  }

  leader_hz = approx_frequency(pcm.samples, 0, SAMPLE_RATE / 10);
  vis_start = (size_t)llround(0.61 * SAMPLE_RATE);
  first_sync = (size_t)llround(0.91 * SAMPLE_RATE);
  sync_hz = approx_frequency(pcm.samples, first_sync, 397);
  if (leader_hz < 1890 || leader_hz > 1910 || first_sync >= pcm.length ||
      vis_start >= pcm.length || sync_hz < 1100 || sync_hz > 1300 ||
      approx_frequency(pcm.samples, vis_start, 1323) < 1170 ||
      approx_frequency(pcm.samples, vis_start, 1323) > 1230 ||
      approx_frequency(pcm.samples, vis_start + 1323, 1323) < 1070 ||
      approx_frequency(pcm.samples, vis_start + 1323, 1323) > 1130 ||
      approx_frequency(pcm.samples, vis_start + 4 * 1323, 1323) < 1270 ||
      approx_frequency(pcm.samples, vis_start + 4 * 1323, 1323) > 1330) {
    fprintf(stderr, "unexpected VIS/header or first-line timing (%d Hz leader, %d Hz sync)\n",
            leader_hz, sync_hz);
    free(pcm.samples);
    return EXIT_FAILURE;
  }

  printf("Robot 36 encoding OK: %zu mono S16 samples, VIS %.1f Hz, sync %d Hz\n",
         pcm.length, (double)leader_hz, sync_hz);
  free(pcm.samples);

  image = gdk_pixbuf_new(GDK_COLORSPACE_RGB, FALSE, 8, 2, 2);
  if (image == NULL) return EXIT_FAILURE;
  gdk_pixbuf_fill(image, 0x4080c0ff);
  if (!check_martin_mode(image, SSTV_MODE_MARTIN_M1, 44, 0.446446) ||
      !check_martin_mode(image, SSTV_MODE_MARTIN_M2, 40, 0.226798)) {
    g_object_unref(image);
    return EXIT_FAILURE;
  }
  g_object_unref(image);
  puts("Martin M1/M2 encoding OK: VIS, timing and RGB scan tones verified");
  if (!check_fskid()) return EXIT_FAILURE;
  puts("FSKID encoding OK: sync, LSB-first tones, callsign and length verified");
  return EXIT_SUCCESS;
}
