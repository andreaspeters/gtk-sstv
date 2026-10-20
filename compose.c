#include "compose.h"

#include <gdk/gdk.h>
#include <pango/pangocairo.h>
#include <string.h>

#define TX_WIDTH 320
#define TX_HEIGHT 240
#define FOOTER_HEIGHT 56
#define THUMB_WIDTH 106
#define THUMB_HEIGHT 80
#define THUMB_MARGIN 8

static cairo_surface_t *pixbuf_to_cairo_surface(GdkPixbuf *pixbuf) {
  cairo_surface_t *surface;
  const guchar *source_pixels;
  guchar *surface_pixels;
  int width, height, source_stride, surface_stride, channels, has_alpha;
  int x, y;

  width = gdk_pixbuf_get_width(pixbuf);
  height = gdk_pixbuf_get_height(pixbuf);
  source_stride = gdk_pixbuf_get_rowstride(pixbuf);
  channels = gdk_pixbuf_get_n_channels(pixbuf);
  has_alpha = gdk_pixbuf_get_has_alpha(pixbuf);
  source_pixels = gdk_pixbuf_get_pixels(pixbuf);
  surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
  if (cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS) {
    cairo_surface_destroy(surface);
    return NULL;
  }

  surface_stride = cairo_image_surface_get_stride(surface);
  surface_pixels = cairo_image_surface_get_data(surface);
  for (y = 0; y < height; ++y) {
    const guchar *source_row = source_pixels + y * source_stride;
    guchar *surface_row = surface_pixels + y * surface_stride;
    for (x = 0; x < width; ++x) {
      const guchar *source_pixel = source_row + x * channels;
      guchar alpha = has_alpha ? source_pixel[3] : 255;
      guchar red = (source_pixel[0] * alpha + 127) / 255;
      guchar green = (source_pixel[1] * alpha + 127) / 255;
      guchar blue = (source_pixel[2] * alpha + 127) / 255;
      guchar *surface_pixel = surface_row + x * 4;

#if G_BYTE_ORDER == G_LITTLE_ENDIAN
      surface_pixel[0] = blue;
      surface_pixel[1] = green;
      surface_pixel[2] = red;
      surface_pixel[3] = alpha;
#else
      surface_pixel[0] = alpha;
      surface_pixel[1] = red;
      surface_pixel[2] = green;
      surface_pixel[3] = blue;
#endif
    }
  }
  cairo_surface_mark_dirty(surface);
  return surface;
}

static GdkPixbuf *pixbuf_from_cairo_surface(cairo_surface_t *surface) {
  GdkPixbuf *pixbuf;
  const guchar *surface_pixels;
  guchar *pixbuf_pixels;
  int width, height, surface_stride, pixbuf_stride, x, y;

  if (cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS ||
      cairo_surface_get_type(surface) != CAIRO_SURFACE_TYPE_IMAGE ||
      cairo_image_surface_get_format(surface) != CAIRO_FORMAT_ARGB32)
    return NULL;

  cairo_surface_flush(surface);
  width = cairo_image_surface_get_width(surface);
  height = cairo_image_surface_get_height(surface);
  surface_stride = cairo_image_surface_get_stride(surface);
  surface_pixels = cairo_image_surface_get_data(surface);
  pixbuf = gdk_pixbuf_new(GDK_COLORSPACE_RGB, TRUE, 8, width, height);
  if (pixbuf == NULL)
    return NULL;

  pixbuf_stride = gdk_pixbuf_get_rowstride(pixbuf);
  pixbuf_pixels = gdk_pixbuf_get_pixels(pixbuf);
  for (y = 0; y < height; ++y) {
    const guchar *surface_row = surface_pixels + y * surface_stride;
    guchar *pixbuf_row = pixbuf_pixels + y * pixbuf_stride;
    for (x = 0; x < width; ++x) {
      const guchar *surface_pixel = surface_row + x * 4;
      guchar *pixbuf_pixel = pixbuf_row + x * 4;
      guchar alpha, red, green, blue;

#if G_BYTE_ORDER == G_LITTLE_ENDIAN
      blue = surface_pixel[0];
      green = surface_pixel[1];
      red = surface_pixel[2];
      alpha = surface_pixel[3];
#else
      alpha = surface_pixel[0];
      red = surface_pixel[1];
      green = surface_pixel[2];
      blue = surface_pixel[3];
#endif
      pixbuf_pixel[0] = alpha == 0 ? 0 : MIN(255, (red * 255 + alpha / 2) / alpha);
      pixbuf_pixel[1] = alpha == 0 ? 0 : MIN(255, (green * 255 + alpha / 2) / alpha);
      pixbuf_pixel[2] = alpha == 0 ? 0 : MIN(255, (blue * 255 + alpha / 2) / alpha);
      pixbuf_pixel[3] = alpha;
    }
  }
  return pixbuf;
}

static void draw_station_info(cairo_t *cr, const char *station_info,
                              guint font_size, const GdkRGBA *station_color,
                              int footer_top, int min_top) {
  PangoLayout *layout;
  PangoFontDescription *font;
  GdkRGBA default_color = {1.0, 1.0, 1.0, 1.0};
  const GdkRGBA *color = station_color != NULL ? station_color : &default_color;
  int available_height = footer_top - min_top;
  int text_width, text_height, text_top;

  if (station_info == NULL || *station_info == '\0' || available_height <= 12)
    return;

  font_size = CLAMP(font_size, 8, 28);
  layout = pango_cairo_create_layout(cr);
  font = pango_font_description_new();
  pango_font_description_set_family(font, "Sans");
  pango_font_description_set_weight(font, PANGO_WEIGHT_BOLD);
  pango_font_description_set_size(font, (int)font_size * PANGO_SCALE);
  pango_layout_set_font_description(layout, font);
  pango_layout_set_width(layout, (TX_WIDTH - 20) * PANGO_SCALE);
  pango_layout_set_wrap(layout, PANGO_WRAP_WORD_CHAR);
  pango_layout_set_single_paragraph_mode(layout, FALSE);
  pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_END);
  pango_layout_set_height(layout, -(available_height - 12) * PANGO_SCALE);
  pango_layout_set_text(layout, station_info, -1);
  pango_layout_get_pixel_size(layout, &text_width, &text_height);
  (void)text_width;
  text_height = CLAMP(text_height, 1, available_height - 12);
  text_top = footer_top - text_height - 12;

  cairo_set_source_rgba(cr, color->red, color->green, color->blue, color->alpha);
  cairo_move_to(cr, 10, text_top + 6);
  pango_cairo_show_layout(cr, layout);

  pango_font_description_free(font);
  g_object_unref(layout);
}

GdkPixbuf *sstv_compose_image(GdkPixbuf *source, const char *my_call,
                              const char *their_call, const char *report,
                              const char *utc_text, const char *message,
                              const char *station_info, guint station_font_size,
                              const GdkRGBA *station_color,
                              GError **error) {
  GdkPixbuf *scaled, *result;
  cairo_surface_t *surface, *scaled_surface;
  cairo_t *cr;
  PangoLayout *layout;
  PangoFontDescription *font;
  char *identity;
  const char *call_a = (my_call != NULL && *my_call != '\0') ? my_call : "NOCALL";
  const char *call_b = (their_call != NULL && *their_call != '\0') ? their_call : "CQ";
  const char *snr = (report != NULL && *report != '\0') ? report : "SNR unavailable";
  const char *utc = (utc_text != NULL && *utc_text != '\0') ? utc_text : "UTC unknown";
  const char *text = (message != NULL) ? message : "";

  if (source == NULL || !GDK_IS_PIXBUF(source) ||
      gdk_pixbuf_get_colorspace(source) != GDK_COLORSPACE_RGB ||
      gdk_pixbuf_get_bits_per_sample(source) != 8 ||
      gdk_pixbuf_get_n_channels(source) < 3) {
    g_set_error_literal(error, g_quark_from_static_string("slowrx-sstv-compose"),
                        1, "Source must be an 8-bit RGB image");
    return NULL;
  }

  scaled = gdk_pixbuf_scale_simple(source, TX_WIDTH, TX_HEIGHT, GDK_INTERP_BILINEAR);
  if (scaled == NULL) {
    g_set_error_literal(error, g_quark_from_static_string("slowrx-sstv-compose"),
                        2, "Unable to scale source image");
    return NULL;
  }

  scaled_surface = pixbuf_to_cairo_surface(scaled);
  if (scaled_surface == NULL) {
    g_set_error_literal(error, g_quark_from_static_string("slowrx-sstv-compose"),
                        4, "Unable to convert source image to Cairo");
    g_object_unref(scaled);
    return NULL;
  }
  surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, TX_WIDTH, TX_HEIGHT);
  cr = cairo_create(surface);
  cairo_set_source_surface(cr, scaled_surface, 0, 0);
  cairo_paint(cr);
  cairo_surface_destroy(scaled_surface);
  draw_station_info(cr, station_info, station_font_size, station_color,
                    TX_HEIGHT - FOOTER_HEIGHT, 0);
  cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.78);
  cairo_rectangle(cr, 0, TX_HEIGHT - FOOTER_HEIGHT, TX_WIDTH, FOOTER_HEIGHT);
  cairo_fill(cr);

  identity = g_strdup_printf("%s DE %s | %s", call_b, call_a, snr);
  layout = pango_cairo_create_layout(cr);
  font = pango_font_description_from_string("Sans Bold 10");
  pango_layout_set_font_description(layout, font);
  pango_layout_set_width(layout, (TX_WIDTH - 12) * PANGO_SCALE);
  pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_END);
  pango_layout_set_single_paragraph_mode(layout, TRUE);

  cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
  cairo_move_to(cr, 6, TX_HEIGHT - FOOTER_HEIGHT + 4);
  pango_layout_set_text(layout, identity, -1);
  pango_cairo_show_layout(cr, layout);

  cairo_move_to(cr, 6, TX_HEIGHT - FOOTER_HEIGHT + 21);
  pango_layout_set_text(layout, utc, -1);
  pango_cairo_show_layout(cr, layout);

  cairo_move_to(cr, 6, TX_HEIGHT - FOOTER_HEIGHT + 38);
  pango_layout_set_text(layout, text, -1);
  pango_cairo_show_layout(cr, layout);

  result = pixbuf_from_cairo_surface(surface);
  pango_font_description_free(font);
  g_object_unref(layout);
  g_free(identity);
  cairo_destroy(cr);
  cairo_surface_destroy(surface);
  g_object_unref(scaled);
  if (result == NULL)
    g_set_error_literal(error, g_quark_from_static_string("slowrx-sstv-compose"),
                        3, "Unable to render image overlay");
  return result;
}

GdkPixbuf *sstv_compose_reply_image(GdkPixbuf *source, const char *my_call,
                                    const char *their_call, const char *report,
                                    const char *utc_text, const char *message,
                                    const char *station_info, guint station_font_size,
                                    const GdkRGBA *station_color,
                                    gboolean image_right, GError **error) {
  GdkPixbuf *scaled, *result;
  cairo_surface_t *surface, *scaled_surface;
  cairo_t *cr;
  PangoLayout *layout;
  PangoFontDescription *font;
  char *identity;
  const char *call_a = (my_call != NULL && *my_call != '\0') ? my_call : "NOCALL";
  const char *call_b = (their_call != NULL && *their_call != '\0') ? their_call : "CQ";
  const char *snr = (report != NULL && *report != '\0') ? report : "SNR unavailable";
  const char *utc = (utc_text != NULL && *utc_text != '\0') ? utc_text : "UTC unknown";
  const char *text = (message != NULL) ? message : "";

  if (source == NULL || !GDK_IS_PIXBUF(source) ||
      gdk_pixbuf_get_colorspace(source) != GDK_COLORSPACE_RGB ||
      gdk_pixbuf_get_bits_per_sample(source) != 8 ||
      gdk_pixbuf_get_n_channels(source) < 3) {
    g_set_error_literal(error, g_quark_from_static_string("slowrx-sstv-compose"),
                        1, "Source must be an 8-bit RGB image");
    return NULL;
  }

  // Keep the received image as a one-third-size inset, not as the canvas background.
  scaled = gdk_pixbuf_scale_simple(source, THUMB_WIDTH, THUMB_HEIGHT,
                                   GDK_INTERP_BILINEAR);
  if (scaled == NULL) {
    g_set_error_literal(error, g_quark_from_static_string("slowrx-sstv-compose"),
                        2, "Unable to scale source image");
    return NULL;
  }

  scaled_surface = pixbuf_to_cairo_surface(scaled);
  if (scaled_surface == NULL) {
    g_set_error_literal(error, g_quark_from_static_string("slowrx-sstv-compose"),
                        4, "Unable to convert source image to Cairo");
    g_object_unref(scaled);
    return NULL;
  }
  surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, TX_WIDTH, TX_HEIGHT);
  cr = cairo_create(surface);
  cairo_set_source_rgb(cr, 0.08, 0.12, 0.18);
  cairo_paint(cr);

  // Calculate position for thumbnail
  int thumb_x = image_right ? TX_WIDTH - THUMB_WIDTH - THUMB_MARGIN : THUMB_MARGIN;
  int thumb_y = THUMB_MARGIN;

  // Draw a light border around the inset for contrast with the canvas.
  cairo_set_source_rgb(cr, 0.86, 0.88, 0.92);
  cairo_rectangle(cr, thumb_x - 2, thumb_y - 2, THUMB_WIDTH + 4, THUMB_HEIGHT + 4);
  cairo_fill(cr);
  cairo_set_source_surface(cr, scaled_surface, thumb_x, thumb_y);
  cairo_paint(cr);
  cairo_surface_destroy(scaled_surface);

  draw_station_info(cr, station_info, station_font_size, station_color,
                    TX_HEIGHT - FOOTER_HEIGHT,
                    THUMB_MARGIN + THUMB_HEIGHT + THUMB_MARGIN);

  // Draw footer with same layout as sstv_compose_image
  cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.78);
  cairo_rectangle(cr, 0, TX_HEIGHT - FOOTER_HEIGHT, TX_WIDTH, FOOTER_HEIGHT);
  cairo_fill(cr);

  identity = g_strdup_printf("%s DE %s | %s", call_b, call_a, snr);
  layout = pango_cairo_create_layout(cr);
  font = pango_font_description_from_string("Sans Bold 10");
  pango_layout_set_font_description(layout, font);
  pango_layout_set_width(layout, (TX_WIDTH - 12) * PANGO_SCALE);
  pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_END);
  pango_layout_set_single_paragraph_mode(layout, TRUE);

  cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
  cairo_move_to(cr, 6, TX_HEIGHT - FOOTER_HEIGHT + 4);
  pango_layout_set_text(layout, identity, -1);
  pango_cairo_show_layout(cr, layout);

  cairo_move_to(cr, 6, TX_HEIGHT - FOOTER_HEIGHT + 21);
  pango_layout_set_text(layout, utc, -1);
  pango_cairo_show_layout(cr, layout);

  cairo_move_to(cr, 6, TX_HEIGHT - FOOTER_HEIGHT + 38);
  pango_layout_set_text(layout, text, -1);
  pango_cairo_show_layout(cr, layout);

  result = pixbuf_from_cairo_surface(surface);
  pango_font_description_free(font);
  g_object_unref(layout);
  g_free(identity);
  cairo_destroy(cr);
  cairo_surface_destroy(surface);
  g_object_unref(scaled);
  if (result == NULL)
    g_set_error_literal(error, g_quark_from_static_string("slowrx-sstv-compose"),
                        3, "Unable to render image overlay");
  return result;
}
