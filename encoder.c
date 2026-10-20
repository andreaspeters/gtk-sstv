#include "encoder.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define SSTV_SAMPLE_RATE 44100
#define ROBOT36_WIDTH 320
#define ROBOT36_HEIGHT 240
#define HALF_US_PER_SECOND 2000000LL
#define PI 3.14159265358979323846

static size_t sample_at_half_us(long long half_us) {
  return (size_t)((half_us * SSTV_SAMPLE_RATE + HALF_US_PER_SECOND / 2) /
                  HALF_US_PER_SECOND);
}

static void write_tone(gint16 *samples, size_t start, size_t end,
                       double frequency, double *phase) {
  size_t i;
  double step = 2.0 * PI * frequency / SSTV_SAMPLE_RATE;

  for (i = start; i < end; ++i) {
    samples[i] = (gint16)(sin(*phase) * 30000.0);
    *phase += step;
    if (*phase >= 2.0 * PI) *phase -= 2.0 * PI;
  }
}

static guchar clip_byte(double value) {
  if (value < 0.0) return 0;
  if (value > 255.0) return 255;
  return (guchar)(value + 0.5);
}

static void pixel_rgb(const guchar *pixel, int channels,
                      guchar *red, guchar *green, guchar *blue) {
  if (channels == 4 && pixel[3] != 255) {
    double alpha = pixel[3] / 255.0;
    *red = clip_byte(pixel[0] * alpha + 255.0 * (1.0 - alpha));
    *green = clip_byte(pixel[1] * alpha + 255.0 * (1.0 - alpha));
    *blue = clip_byte(pixel[2] * alpha + 255.0 * (1.0 - alpha));
  } else {
    *red = pixel[0];
    *green = pixel[1];
    *blue = pixel[2];
  }
}

static void rgb_yuv(guchar red, guchar green, guchar blue,
                    double *y, double *r_y, double *b_y) {
  *y = 0.299 * red + 0.587 * green + 0.114 * blue;
  *r_y = 128.0 + (red - *y) / 1.4;
  *b_y = 128.0 + (blue - *y) / 1.77;
}

static void set_error(GError **error, int code, const char *message) {
  if (error != NULL)
    g_set_error_literal(error, g_quark_from_static_string("slowrx-sstv-encoder"),
                        code, message);
}

gboolean sstv_encode_fskid(const gchar *callsign, SstvPcm *pcm, GError **error) {
  gchar *uppercase;
  size_t call_length, byte_count, bit_count, sample_count;
  size_t byte_index, bit_index;
  double phase = 0.0;

  if (pcm == NULL) {
    set_error(error, 6, "Missing FSKID output buffer");
    return FALSE;
  }
  pcm->samples = NULL;
  pcm->length = 0;
  if (callsign == NULL) {
    set_error(error, 7, "Missing FSKID callsign");
    return FALSE;
  }

  uppercase = g_ascii_strup(callsign, -1);
  call_length = strlen(uppercase);
  if (call_length == 0 || call_length > 10) {
    g_free(uppercase);
    set_error(error, 8, "FSKID callsign must contain 1 to 10 characters");
    return FALSE;
  }
  for (byte_index = 0; byte_index < call_length; ++byte_index) {
    guchar ch = (guchar)uppercase[byte_index];
    if (!g_ascii_isalnum(ch) && ch != '/' && ch != '-') {
      g_free(uppercase);
      set_error(error, 9, "FSKID callsign may contain only letters, digits, '/' or '-'");
      return FALSE;
    }
  }

  byte_count = call_length + 3;
  bit_count = byte_count * 6;
  sample_count = sample_at_half_us((long long)bit_count * 44000LL);
  pcm->samples = malloc(sample_count * sizeof(*pcm->samples));
  if (pcm->samples == NULL) {
    g_free(uppercase);
    set_error(error, 10, "Unable to allocate FSKID audio buffer");
    return FALSE;
  }
  pcm->length = sample_count;

  for (byte_index = 0; byte_index < byte_count; ++byte_index) {
    guchar value;
    if (byte_index == 0) value = 0x20;
    else if (byte_index == 1) value = 0x2a;
    else if (byte_index == byte_count - 1) value = 0x01;
    else value = (guchar)uppercase[byte_index - 2] - 0x20;

    for (bit_index = 0; bit_index < 6; ++bit_index) {
      size_t symbol = byte_index * 6 + bit_index;
      size_t start = sample_at_half_us((long long)symbol * 44000LL);
      size_t end = sample_at_half_us((long long)(symbol + 1) * 44000LL);
      double frequency = ((value >> bit_index) & 1U) ? 1900.0 : 2100.0;
      write_tone(pcm->samples, start, end, frequency, &phase);
    }
  }

  g_free(uppercase);
  return TRUE;
}

gboolean sstv_encode_robot36(GdkPixbuf *image, SstvPcm *pcm, GError **error) {
  GdkPixbuf *scaled;
  const guchar *pixels;
  int rowstride, channels, x, y, bit;
  size_t count, line_start, start, end;
  double phase = 0.0;
  long long half_us;

  if (pcm == NULL) {
    set_error(error, 1, "Missing output buffer");
    return FALSE;
  }
  pcm->samples = NULL;
  pcm->length = 0;
  if (image == NULL || !GDK_IS_PIXBUF(image) ||
      gdk_pixbuf_get_colorspace(image) != GDK_COLORSPACE_RGB ||
      gdk_pixbuf_get_bits_per_sample(image) != 8 ||
      gdk_pixbuf_get_n_channels(image) < 3) {
    set_error(error, 2, "Input must be an 8-bit RGB image");
    return FALSE;
  }

  scaled = gdk_pixbuf_scale_simple(image, ROBOT36_WIDTH, ROBOT36_HEIGHT,
                                   GDK_INTERP_BILINEAR);
  if (scaled == NULL) {
    set_error(error, 3, "Unable to scale image for Robot 36");
    return FALSE;
  }

  /* VIS 8: 300 ms leader, 10 ms break, 300 ms leader, then start,
   * seven data bits, even parity, and stop (30 ms per bit). */
  count = sample_at_half_us(73820000LL);
  pcm->samples = malloc(count * sizeof(*pcm->samples));
  if (pcm->samples == NULL) {
    g_object_unref(scaled);
    set_error(error, 4, "Unable to allocate SSTV audio buffer");
    return FALSE;
  }
  pcm->length = count;

  half_us = 0;
  start = sample_at_half_us(half_us);
  half_us += 600000LL;
  end = sample_at_half_us(half_us);
  write_tone(pcm->samples, start, end, 1900.0, &phase);
  start = end;
  half_us += 20000LL;
  end = sample_at_half_us(half_us);
  write_tone(pcm->samples, start, end, 1200.0, &phase);
  start = end;
  half_us += 600000LL;
  end = sample_at_half_us(half_us);
  write_tone(pcm->samples, start, end, 1900.0, &phase);

  for (bit = 0; bit < 10; ++bit) {
    int value;
    double frequency;
    start = end;
    half_us += 60000LL;
    end = sample_at_half_us(half_us);
    if (bit == 0 || bit == 9) {
      frequency = 1200.0;
    } else {
      if (bit <= 7) value = (0x08 >> (bit - 1)) & 1;
      else value = 1; /* even parity for VIS 08h */
      frequency = value ? 1300.0 : 1100.0;
    }
    write_tone(pcm->samples, start, end, frequency, &phase);
  }

  pixels = gdk_pixbuf_get_pixels(scaled);
  rowstride = gdk_pixbuf_get_rowstride(scaled);
  channels = gdk_pixbuf_get_n_channels(scaled);
  for (y = 0; y < ROBOT36_HEIGHT; ++y) {
    line_start = sample_at_half_us(1820000LL + (long long)y * 300000LL);

    /* 9 ms sync (1200 Hz), 3 ms porch (1500 Hz). */
    start = line_start;
    end = sample_at_half_us(1820000LL + (long long)y * 300000LL + 18000LL);
    write_tone(pcm->samples, start, end, 1200.0, &phase);
    start = end;
    end = sample_at_half_us(1820000LL + (long long)y * 300000LL + 24000LL);
    write_tone(pcm->samples, start, end, 1500.0, &phase);

    /* Y: 320 pixels at 275 us each (88 ms). */
    for (x = 0; x < ROBOT36_WIDTH; ++x) {
      guchar red, green, blue;
      double luminance, red_y, blue_y;
      const guchar *pixel = pixels + y * rowstride + x * channels;
      pixel_rgb(pixel, channels, &red, &green, &blue);
      rgb_yuv(red, green, blue, &luminance, &red_y, &blue_y);
      start = sample_at_half_us(1820000LL + (long long)y * 300000LL +
                                24000LL + (long long)x * 550LL);
      end = sample_at_half_us(1820000LL + (long long)y * 300000LL +
                              24000LL + (long long)(x + 1) * 550LL);
      write_tone(pcm->samples, start, end,
                 1500.0 + luminance * 800.0 / 255.0, &phase);
    }

    /* 6 ms separator, then 320 chroma samples at 137.5 us. */
    start = sample_at_half_us(1820000LL + (long long)y * 300000LL + 200000LL);
    end = sample_at_half_us(1820000LL + (long long)y * 300000LL + 212000LL);
    write_tone(pcm->samples, start, end, 1500.0, &phase);
    for (x = 0; x < ROBOT36_WIDTH; ++x) {
      guchar red, green, blue;
      double luminance, red_y, blue_y, chroma;
      const guchar *pixel = pixels + y * rowstride + x * channels;
      pixel_rgb(pixel, channels, &red, &green, &blue);
      rgb_yuv(red, green, blue, &luminance, &red_y, &blue_y);
      chroma = (y % 2 == 0) ? red_y : blue_y;
      start = sample_at_half_us(1820000LL + (long long)y * 300000LL +
                                212000LL + (long long)x * 275LL);
      end = sample_at_half_us(1820000LL + (long long)y * 300000LL +
                              212000LL + (long long)(x + 1) * 275LL);
      write_tone(pcm->samples, start, end,
                 1500.0 + chroma * 800.0 / 255.0, &phase);
    }
  }

  g_object_unref(scaled);
  return TRUE;
}

static gboolean encode_martin(GdkPixbuf *image, SstvMode mode,
                              SstvPcm *pcm, GError **error) {
  const int width = 320;
  const int height = 256;
  const long long line_start_half_us = 1820000LL;
  const long long sync_half_us = 9724LL;
  const long long porch_half_us = 1144LL;
  const long long separator_half_us = 1144LL;
  const unsigned int vis = mode == SSTV_MODE_MARTIN_M1 ? 44U : 40U;
  const double scan_us = mode == SSTV_MODE_MARTIN_M1 ? 146432.0 : 73216.0;
  const double pixel_half_us = (scan_us * 2.0) / width;
  const long long scan_half_us = (long long)llround(scan_us * 2.0);
  const long long line_half_us = sync_half_us + porch_half_us +
      3LL * (scan_half_us + separator_half_us);
  GdkPixbuf *scaled;
  const guchar *pixels;
  int rowstride, channels, x, y, bit, color;
  size_t count, start, end;
  long long half_us;
  double phase = 0.0;

  if (pcm == NULL) {
    set_error(error, 1, "Missing output buffer");
    return FALSE;
  }
  pcm->samples = NULL;
  pcm->length = 0;
  if (image == NULL || !GDK_IS_PIXBUF(image) ||
      gdk_pixbuf_get_colorspace(image) != GDK_COLORSPACE_RGB ||
      gdk_pixbuf_get_bits_per_sample(image) != 8 ||
      gdk_pixbuf_get_n_channels(image) < 3) {
    set_error(error, 2, "Input must be an 8-bit RGB image");
    return FALSE;
  }

  scaled = gdk_pixbuf_scale_simple(image, width, height, GDK_INTERP_BILINEAR);
  if (scaled == NULL) {
    set_error(error, 3, "Unable to scale image for Martin SSTV");
    return FALSE;
  }
  count = sample_at_half_us(line_start_half_us + height * line_half_us);
  pcm->samples = malloc(count * sizeof(*pcm->samples));
  if (pcm->samples == NULL) {
    g_object_unref(scaled);
    set_error(error, 4, "Unable to allocate SSTV audio buffer");
    return FALSE;
  }
  pcm->length = count;

  /* VIS 8-bit leader and 7-bit mode code, transmitted least-significant bit first. */
  half_us = 0;
  start = sample_at_half_us(half_us);
  half_us += 600000LL;
  end = sample_at_half_us(half_us);
  write_tone(pcm->samples, start, end, 1900.0, &phase);
  start = end;
  half_us += 20000LL;
  end = sample_at_half_us(half_us);
  write_tone(pcm->samples, start, end, 1200.0, &phase);
  start = end;
  half_us += 600000LL;
  end = sample_at_half_us(half_us);
  write_tone(pcm->samples, start, end, 1900.0, &phase);
  for (bit = 0; bit < 10; ++bit) {
    int value = 0;
    double frequency;
    start = end;
    half_us += 60000LL;
    end = sample_at_half_us(half_us);
    if (bit == 0 || bit == 9) {
      frequency = 1200.0;
    } else if (bit <= 7) {
      value = (vis >> (bit - 1)) & 1U;
      frequency = value ? 1300.0 : 1100.0;
    } else {
      int ones = 0;
      int code_bit;
      for (code_bit = 0; code_bit < 7; ++code_bit)
        ones += (vis >> code_bit) & 1U;
      value = ones & 1;
      frequency = value ? 1300.0 : 1100.0;
    }
    write_tone(pcm->samples, start, end, frequency, &phase);
  }

  pixels = gdk_pixbuf_get_pixels(scaled);
  rowstride = gdk_pixbuf_get_rowstride(scaled);
  channels = gdk_pixbuf_get_n_channels(scaled);
  for (y = 0; y < height; ++y) {
    long long line_half = line_start_half_us + (long long)y * line_half_us;
    long long component_half = line_half + sync_half_us + porch_half_us;
    start = sample_at_half_us(line_half);
    end = sample_at_half_us(line_half + sync_half_us);
    write_tone(pcm->samples, start, end, 1200.0, &phase);
    start = end;
    end = sample_at_half_us(line_half + sync_half_us + porch_half_us);
    write_tone(pcm->samples, start, end, 1500.0, &phase);

    /* Martin transmits each line in Green, Blue, Red order. */
    for (color = 0; color < 3; ++color) {
      int channel = color == 0 ? 1 : (color == 1 ? 2 : 0);
      for (x = 0; x < width; ++x) {
        guchar red, green, blue, value;
        const guchar *pixel = pixels + y * rowstride + x * channels;
        long long pixel_start = component_half + (long long)llround(x * pixel_half_us);
        long long pixel_end = component_half + (long long)llround((x + 1) * pixel_half_us);
        pixel_rgb(pixel, channels, &red, &green, &blue);
        value = channel == 0 ? red : (channel == 1 ? green : blue);
        start = sample_at_half_us(pixel_start);
        end = sample_at_half_us(pixel_end);
        write_tone(pcm->samples, start, end,
                   1500.0 + value * 800.0 / 255.0, &phase);
      }
      component_half += scan_half_us;
      start = sample_at_half_us(component_half);
      component_half += separator_half_us;
      end = sample_at_half_us(component_half);
      write_tone(pcm->samples, start, end, 1500.0, &phase);
    }
  }

  g_object_unref(scaled);
  return TRUE;
}

gboolean sstv_encode_mode(GdkPixbuf *image, SstvMode mode,
                          SstvPcm *pcm, GError **error) {
  switch (mode) {
    case SSTV_MODE_ROBOT36:
      return sstv_encode_robot36(image, pcm, error);
    case SSTV_MODE_MARTIN_M1:
    case SSTV_MODE_MARTIN_M2:
      return encode_martin(image, mode, pcm, error);
    default:
      if (pcm != NULL) {
        pcm->samples = NULL;
        pcm->length = 0;
      }
      set_error(error, 5, "Unsupported SSTV transmit mode");
      return FALSE;
  }
}

const gchar *sstv_mode_name(SstvMode mode) {
  switch (mode) {
    case SSTV_MODE_ROBOT36: return "Robot 36";
    case SSTV_MODE_MARTIN_M1: return "Martin M1";
    case SSTV_MODE_MARTIN_M2: return "Martin M2";
    default: return "Unknown SSTV mode";
  }
}
