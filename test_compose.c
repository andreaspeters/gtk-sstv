#include "compose.h"

#include <stdio.h>
#include <stdlib.h>

#define IMAGE_WIDTH 320
#define IMAGE_HEIGHT 240

static GdkPixbuf *new_source(guint32 fill) {
  GdkPixbuf *source = gdk_pixbuf_new(GDK_COLORSPACE_RGB, FALSE, 8,
                                     IMAGE_WIDTH, IMAGE_HEIGHT);
  if (source != NULL) gdk_pixbuf_fill(source, fill);
  return source;
}

static int count_red_pixels(GdkPixbuf *image, int y_start, int y_end) {
  guchar *pixels = gdk_pixbuf_get_pixels(image);
  int rowstride = gdk_pixbuf_get_rowstride(image);
  int channels = gdk_pixbuf_get_n_channels(image);
  int count = 0;
  int x, y;

  for (y = y_start; y < y_end; ++y) {
    for (x = 0; x < IMAGE_WIDTH; ++x) {
      guchar *pixel = pixels + y * rowstride + x * channels;
      if (pixel[0] > 120 && pixel[1] < 100 && pixel[2] < 100) ++count;
    }
  }
  return count;
}

static int count_bright_pixels(GdkPixbuf *image, int y_start, int y_end) {
  guchar *pixels = gdk_pixbuf_get_pixels(image);
  int rowstride = gdk_pixbuf_get_rowstride(image);
  int channels = gdk_pixbuf_get_n_channels(image);
  int count = 0;
  int x, y;

  for (y = y_start; y < y_end; ++y) {
    for (x = 0; x < IMAGE_WIDTH; ++x) {
      guchar *pixel = pixels + y * rowstride + x * channels;
      if (pixel[0] > 180 && pixel[1] > 180 && pixel[2] > 180) ++count;
    }
  }
  return count;
}

static int fail(const char *message, GError *error) {
  fprintf(stderr, "%s%s%s\n", message,
          error != NULL ? ": " : "", error != NULL ? error->message : "");
  if (error != NULL) g_error_free(error);
  return EXIT_FAILURE;
}

int main(void) {
  const char *station_text = "OPERATOR: N0CALL\nQTH: TEST CITY\nRIG: 5 W HF";
  GdkRGBA red = {1.0, 0.0, 0.0, 1.0};
  GError *error = NULL;
  GdkPixbuf *source, *large, *small, *reply, *reply_right, *received;
  GdkPixbuf *fallback_reply, *placeholder, *expected_inset;
  guchar *expected_pixels;
  int expected_rowstride, expected_channels, channel;
  int large_red, small_red;
  guchar *pixels;
  int rowstride, channels;

  large = sstv_compose_image(NULL, "N0CALL", "K1ABC", "SNR +12 dB",
                             "2026-10-20 12:34 UTC", "JO62QM", "TNX FER QSO", "", 14,
                             &red, &error);
  if (large != NULL || error == NULL) {
    if (large != NULL) g_object_unref(large);
    return fail("null source must be rejected", error);
  }
  g_error_free(error);
  error = NULL;

  source = new_source(0x406080ff);
  if (source == NULL) return fail("unable to allocate source image", NULL);
  large = sstv_compose_image(source, "N0CALL", "K1ABC", "SNR +12 dB",
                             "2026-10-20 12:34 UTC", "JO62QM", "TNX FER QSO",
                             station_text, 18, &red, &error);
  if (large == NULL) {
    g_object_unref(source);
    return fail("compose with multiline station information failed", error);
  }
  if (gdk_pixbuf_get_width(large) != IMAGE_WIDTH ||
      gdk_pixbuf_get_height(large) != IMAGE_HEIGHT) {
    g_object_unref(source);
    g_object_unref(large);
    return fail("unexpected composed image dimensions", NULL);
  }
  large_red = count_red_pixels(large, 80, IMAGE_HEIGHT - 56);
  if (large_red < 80) {
    g_object_unref(source);
    g_object_unref(large);
    return fail("custom red station text was not rendered", NULL);
  }
  pixels = gdk_pixbuf_get_pixels(large);
  rowstride = gdk_pixbuf_get_rowstride(large);
  channels = gdk_pixbuf_get_n_channels(large);
  {
    guchar *background_sample = pixels + 120 * rowstride + 310 * channels;
    if (abs((int)background_sample[0] - 0x40) > 2 ||
        abs((int)background_sample[1] - 0x60) > 2 ||
        abs((int)background_sample[2] - 0x80) > 2) {
      g_object_unref(source);
      g_object_unref(large);
      return fail("multiline station text has an unwanted background", NULL);
    }
  }
  if (count_bright_pixels(large, IMAGE_HEIGHT - 56, IMAGE_HEIGHT) < 20) {
    g_object_unref(source);
    g_object_unref(large);
    return fail("fixed footer text was not rendered", NULL);
  }

  small = sstv_compose_image(source, "N0CALL", "K1ABC", "SNR +12 dB",
                             "2026-10-20 12:34 UTC", "JO62QM", "TNX FER QSO",
                             station_text, 8, &red, &error);
  if (small == NULL) {
    g_object_unref(source);
    g_object_unref(large);
    return fail("compose with small station font failed", error);
  }
  small_red = count_red_pixels(small, 80, IMAGE_HEIGHT - 56);
  if (large_red <= small_red) {
    g_object_unref(source);
    g_object_unref(large);
    g_object_unref(small);
    return fail("larger station font did not render larger text", NULL);
  }
  g_object_unref(small);
  g_object_unref(large);
  g_object_unref(source);
  source = new_source(0x0000ffff);
  received = new_source(0xff0000ff);
  if (source == NULL || received == NULL) {
    g_clear_object(&source);
    g_clear_object(&received);
    return fail("unable to allocate reply images", NULL);
  }
  reply = sstv_compose_reply_image(source, received, "N0CALL", "K1ABC",
                                   "SNR +12 dB", "2026-10-20 12:34 UTC",
                                   "JO62QM", "TNX FER QSO", "", 14, &red,
                                   FALSE, &error);
  if (reply == NULL) {
    g_object_unref(source);
    g_object_unref(received);
    return fail("left reply composition failed", error);
  }
  pixels = gdk_pixbuf_get_pixels(reply);
  rowstride = gdk_pixbuf_get_rowstride(reply);
  channels = gdk_pixbuf_get_n_channels(reply);
  if (pixels[120 * rowstride + 160 * channels] > 80 ||
      pixels[120 * rowstride + 160 * channels + 1] > 80 ||
      pixels[120 * rowstride + 160 * channels + 2] < 180 ||
      pixels[20 * rowstride + 20 * channels] < 180 ||
      pixels[20 * rowstride + 20 * channels + 1] > 80 ||
      pixels[20 * rowstride + 20 * channels + 2] > 80) {
    g_object_unref(source);
    g_object_unref(received);
    g_object_unref(reply);
    return fail("left reply must preserve the base and overlay the received image", NULL);
  }
  g_object_unref(reply);

  reply_right = sstv_compose_reply_image(source, received, "N0CALL", "K1ABC",
                                         "SNR +12 dB", "2026-10-20 12:34 UTC",
                                         "JO62QM", "TNX FER QSO", "", 14, &red,
                                         TRUE, &error);
  if (reply_right == NULL) {
    g_object_unref(source);
    g_object_unref(received);
    return fail("right reply composition failed", error);
  }
  pixels = gdk_pixbuf_get_pixels(reply_right);
  rowstride = gdk_pixbuf_get_rowstride(reply_right);
  channels = gdk_pixbuf_get_n_channels(reply_right);
  if (pixels[120 * rowstride + 160 * channels] > 80 ||
      pixels[120 * rowstride + 160 * channels + 1] > 80 ||
      pixels[120 * rowstride + 160 * channels + 2] < 180 ||
      pixels[20 * rowstride + 300 * channels] < 180 ||
      pixels[20 * rowstride + 300 * channels + 1] > 80 ||
      pixels[20 * rowstride + 300 * channels + 2] > 80) {
    g_object_unref(source);
    g_object_unref(received);
    g_object_unref(reply_right);
    return fail("right reply must preserve the base and overlay the received image", NULL);
  }

  placeholder = gdk_pixbuf_new_from_file("./testbild.png", &error);
  if (placeholder == NULL) {
    g_object_unref(source);
    g_object_unref(received);
    g_object_unref(reply_right);
    return fail("unable to load reply placeholder", error);
  }
  expected_inset = gdk_pixbuf_scale_simple(placeholder, 106, 80,
                                           GDK_INTERP_BILINEAR);
  if (expected_inset == NULL) {
    g_object_unref(source);
    g_object_unref(received);
    g_object_unref(reply_right);
    g_object_unref(placeholder);
    return fail("unable to scale expected reply placeholder", NULL);
  }

  fallback_reply = sstv_compose_reply_image(
      source, NULL, "N0CALL", "", "", "2026-10-20 12:34 UTC", "JO62QM",
      "TNX FER QSO", "", 14, &red, FALSE, &error);
  if (fallback_reply == NULL) {
    g_object_unref(source);
    g_object_unref(received);
    g_object_unref(reply_right);
    g_object_unref(placeholder);
    g_object_unref(expected_inset);
    return fail("missing received image must use the testbild placeholder", error);
  }
  pixels = gdk_pixbuf_get_pixels(fallback_reply);
  rowstride = gdk_pixbuf_get_rowstride(fallback_reply);
  channels = gdk_pixbuf_get_n_channels(fallback_reply);
  expected_pixels = gdk_pixbuf_get_pixels(expected_inset);
  expected_rowstride = gdk_pixbuf_get_rowstride(expected_inset);
  expected_channels = gdk_pixbuf_get_n_channels(expected_inset);
  for (channel = 0; channel < 3; ++channel) {
    int actual = pixels[48 * rowstride + 61 * channels + channel];
    int expected = expected_pixels[40 * expected_rowstride +
                                   53 * expected_channels + channel];
    if (abs(actual - expected) > 5) {
      g_object_unref(source);
      g_object_unref(received);
      g_object_unref(reply_right);
      g_object_unref(fallback_reply);
      g_object_unref(placeholder);
      g_object_unref(expected_inset);
      return fail("missing received image must render testbild.png", NULL);
    }
  }

  g_object_unref(fallback_reply);
  g_object_unref(placeholder);
  g_object_unref(expected_inset);
  g_object_unref(source);
  g_object_unref(received);
  g_object_unref(reply_right);
  puts("All compose tests passed: validation, multiline color/size, footer, and reply insets.");
  return EXIT_SUCCESS;
}