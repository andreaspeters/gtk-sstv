#include "compose.h"

#include <stdio.h>
#include <stdlib.h>

int main(void) {
  GdkPixbuf *source, *composed;
  GError *error = NULL;
  guchar *pixels;
  int rowstride, channels, x, y, bright_pixels = 0;

  composed = sstv_compose_image(NULL, "N0CALL", "K1ABC", "SNR +12 dB",
                                "2026-10-20 12:34 UTC", "TNX FER QSO", &error);
  if (composed != NULL || error == NULL) {
    fprintf(stderr, "null source must be rejected\n");
    return EXIT_FAILURE;
  }
  g_error_free(error);
  error = NULL;

  source = gdk_pixbuf_new(GDK_COLORSPACE_RGB, FALSE, 8, 320, 240);
  if (source == NULL) return EXIT_FAILURE;
  gdk_pixbuf_fill(source, 0x406080ff);
  composed = sstv_compose_image(source, "N0CALL", "K1ABC", "SNR +12 dB",
                                "2026-10-20 12:34 UTC", "TNX FER QSO", &error);
  g_object_unref(source);
  if (composed == NULL) {
    fprintf(stderr, "compose failed: %s\n", error ? error->message : "unknown error");
    if (error != NULL) g_error_free(error);
    return EXIT_FAILURE;
  }

  if (gdk_pixbuf_get_width(composed) != 320 || gdk_pixbuf_get_height(composed) != 240) {
    fprintf(stderr, "unexpected composed image dimensions\n");
    g_object_unref(composed);
    return EXIT_FAILURE;
  }
  pixels = gdk_pixbuf_get_pixels(composed);
  rowstride = gdk_pixbuf_get_rowstride(composed);
  channels = gdk_pixbuf_get_n_channels(composed);
  for (y = 184; y < 240; ++y) {
    for (x = 0; x < 320; ++x) {
      guchar *pixel = pixels + y * rowstride + x * channels;
      if (pixel[0] > 180 && pixel[1] > 180 && pixel[2] > 180) bright_pixels++;
    }
  }
  if (bright_pixels < 20) {
    fprintf(stderr, "footer text was not rendered\n");
    g_object_unref(composed);
    return EXIT_FAILURE;
  }

  printf("Image composition OK: 320x240 with %d bright text pixels in footer\n",
         bright_pixels);
  g_object_unref(composed);
  return EXIT_SUCCESS;
}
