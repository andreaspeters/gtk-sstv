#ifndef SLOWRX_COMPOSE_H
#define SLOWRX_COMPOSE_H

#include <gdk-pixbuf/gdk-pixbuf.h>
#include <gdk/gdk.h>

/* Return a 320x240 RGB SSTV image with a readable station/UTC/message footer. */
GdkPixbuf *sstv_compose_image(GdkPixbuf *source, const char *my_call,
                              const char *their_call, const char *report,
                              const char *utc_text, const char *message,
                              const char *station_info, guint station_font_size,
                              const GdkRGBA *station_color,
                              GError **error);

/* Return a 320x240 RGB SSTV image with reply thumbnail in corner (FALSE=top-left, TRUE=top-right). */
GdkPixbuf *sstv_compose_reply_image(GdkPixbuf *source, const char *my_call,
                                    const char *their_call, const char *report,
                                    const char *utc_text, const char *message,
                                    const char *station_info, guint station_font_size,
                                    const GdkRGBA *station_color,
                                    gboolean image_right, GError **error);

#endif
