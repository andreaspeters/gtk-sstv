#ifndef SLOWRX_COMPOSE_H
#define SLOWRX_COMPOSE_H

#include <gdk-pixbuf/gdk-pixbuf.h>

/* Return a 320x240 RGB SSTV image with a readable station/UTC/message footer. */
GdkPixbuf *sstv_compose_image(GdkPixbuf *source, const char *my_call,
                              const char *their_call, const char *report,
                              const char *utc_text, const char *message,
                              GError **error);

#endif
