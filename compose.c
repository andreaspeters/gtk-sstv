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

GdkPixbuf *sstv_compose_image(GdkPixbuf *source, const char *my_call,
                              const char *their_call, const char *report,
                              const char *utc_text, const char *message,
                              GError **error) {
  GdkPixbuf *scaled, *result;
  cairo_surface_t *surface;
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

  surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, TX_WIDTH, TX_HEIGHT);
  cr = cairo_create(surface);
  gdk_cairo_set_source_pixbuf(cr, scaled, 0, 0);
  cairo_paint(cr);
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

  result = gdk_pixbuf_get_from_surface(surface, 0, 0, TX_WIDTH, TX_HEIGHT);
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
                                    gboolean image_right, GError **error) {
  GdkPixbuf *scaled, *result;
  cairo_surface_t *surface;
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
  gdk_cairo_set_source_pixbuf(cr, scaled, thumb_x, thumb_y);
  cairo_paint(cr);

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

  result = gdk_pixbuf_get_from_surface(surface, 0, 0, TX_WIDTH, TX_HEIGHT);
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
