#ifndef SLOWRX_TXDIALOG_H
#define SLOWRX_TXDIALOG_H

#include <gtk/gtk.h>

GtkWidget *sstv_tx_panel_new(GtkWindow *parent, GKeyFile *config);
void sstv_tx_panel_set_received(GtkWidget *panel, GdkPixbuf *received,
                                const char *received_call, double snr_db,
                                gboolean snr_valid);

#endif
