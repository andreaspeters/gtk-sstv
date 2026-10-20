#include "compose.h"

#include <stdio.h>
#include <stdlib.h>

int main(void) {
  GdkPixbuf *source, *composed, *composed_right;
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

  // Test the new reply image function with top-left position
  g_object_unref(composed);
  source = gdk_pixbuf_new(GDK_COLORSPACE_RGB, FALSE, 8, 320, 240);
  if (source == NULL) return EXIT_FAILURE;
  gdk_pixbuf_fill(source, 0xff0000ff);
  composed = sstv_compose_reply_image(source, "N0CALL", "K1ABC", "SNR +12 dB",
                                      "2026-10-20 12:34 UTC", "TNX FER QSO", FALSE, &error);
  g_object_unref(source);
  if (composed == NULL) {
    fprintf(stderr, "reply compose failed: %s\n", error ? error->message : "unknown error");
    if (error != NULL) g_error_free(error);
    return EXIT_FAILURE;
  }

  if (gdk_pixbuf_get_width(composed) != 320 || gdk_pixbuf_get_height(composed) != 240) {
    fprintf(stderr, "unexpected composed reply image dimensions\n");
    g_object_unref(composed);
    return EXIT_FAILURE;
  }
  pixels = gdk_pixbuf_get_pixels(composed);
  rowstride = gdk_pixbuf_get_rowstride(composed);
  channels = gdk_pixbuf_get_n_channels(composed);
  if (pixels[10 * rowstride + 10 * channels] < 200 ||
      pixels[10 * rowstride + 10 * channels + 1] > 80 ||
      pixels[10 * rowstride + 250 * channels] > 80) {
    fprintf(stderr, "reply thumbnail is not isolated in the top-left corner\n");
    g_object_unref(composed);
    return EXIT_FAILURE;
  }

  // Test the new reply image function with top-right position
  source = gdk_pixbuf_new(GDK_COLORSPACE_RGB, FALSE, 8, 320, 240);
  if (source == NULL) return EXIT_FAILURE;
  gdk_pixbuf_fill(source, 0xff0000ff);
  composed_right = sstv_compose_reply_image(source, "N0CALL", "K1ABC", "SNR +12 dB",
                                            "2026-10-20 12:34 UTC", "TNX FER QSO", TRUE, &error);
  g_object_unref(source);
  if (composed_right == NULL) {
    fprintf(stderr, "reply compose right failed: %s\n", error ? error->message : "unknown error");
    if (error != NULL) g_error_free(error);
    return EXIT_FAILURE;
  }

  if (gdk_pixbuf_get_width(composed_right) != 320 || gdk_pixbuf_get_height(composed_right) != 240) {
    fprintf(stderr, "unexpected composed reply image right dimensions\n");
    g_object_unref(composed_right);
    return EXIT_FAILURE;
  }
  pixels = gdk_pixbuf_get_pixels(composed_right);
  rowstride = gdk_pixbuf_get_rowstride(composed_right);
  channels = gdk_pixbuf_get_n_channels(composed_right);
  if (pixels[10 * rowstride + 300 * channels] < 200 ||
      pixels[10 * rowstride + 300 * channels + 1] > 80 ||
      pixels[10 * rowstride + 20 * channels] > 80) {
    fprintf(stderr, "reply thumbnail is not isolated in the top-right corner\n");
    g_object_unref(composed_right);
    g_object_unref(composed);
    return EXIT_FAILURE;
  }

  printf("All tests passed: 320x240 images with footer and thumbnail in specified corners\n");
  g_object_unref(composed);
  g_object_unref(composed_right);
  return EXIT_SUCCESS;
}
