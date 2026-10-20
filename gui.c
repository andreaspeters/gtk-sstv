#include <stdlib.h>
#include <gtk/gtk.h>
#include <alsa/asoundlib.h>
#include <math.h>

#include <fftw3.h>

#include "common.h"
#include "txdialog.h"
#include "adifudp.h"

#define ADIF_UDP_HOST "127.0.0.1"
#define ADIF_UDP_PORT 2333

static void evt_answer(GtkButton *button, gpointer user_data) {
  GtkNotebook *notebook = GTK_NOTEBOOK(user_data);
  (void)button;

  if (!CurrentPic.ReceivedValid || pixbuf_rx == NULL || gui.tx_panel == NULL)
    return;

  sstv_tx_panel_prepare_reply(gui.tx_panel, pixbuf_rx, CurrentPic.RemoteCall,
                              CurrentPic.SnrDb, CurrentPic.SnrValid);
  gtk_notebook_set_current_page(notebook, 1);
}

static void qso_timestamp(gchar **qso_date, gchar **time_on) {
  const gchar *stamp = CurrentPic.timestr;
  gboolean valid = stamp != NULL && strlen(stamp) >= 15 && stamp[8] == '-';
  guint i;

  if (valid) {
    for (i = 0; i < 8; ++i)
      if (!g_ascii_isdigit(stamp[i])) valid = FALSE;
    for (i = 9; i < 15; ++i)
      if (!g_ascii_isdigit(stamp[i])) valid = FALSE;
  }
  if (valid) {
    *qso_date = g_strndup(stamp, 8);
    *time_on = g_strndup(stamp + 9, 6);
  } else {
    GDateTime *now = g_date_time_new_now_utc();
    *qso_date = g_date_time_format(now, "%Y%m%d");
    *time_on = g_date_time_format(now, "%H%M%S");
    g_date_time_unref(now);
  }
}

static void evt_log_adif(GtkButton *button, gpointer user_data) {
  gchar *qso_date, *time_on, *station_call, *comment, *record, *status;
  GError *error = NULL;
  (void)button;
  (void)user_data;

  if (!CurrentPic.ReceivedValid || CurrentPic.RemoteCall[0] == '\0') {
    gtk_statusbar_push(GTK_STATUSBAR(gui.statusbar), 0,
                       "Cannot log: no valid received SSTV callsign is available.");
    return;
  }

  qso_timestamp(&qso_date, &time_on);
  station_call = g_key_file_get_string(config, "slowrx", "tx_callsign", NULL);
  if (CurrentPic.SnrValid && isfinite(CurrentPic.SnrDb))
    comment = g_strdup_printf("SSTV mode %s; RX SNR %+.1f dB",
                              ModeSpec[CurrentPic.Mode].Name, CurrentPic.SnrDb);
  else
    comment = g_strdup_printf("SSTV mode %s; RX SNR unavailable",
                              ModeSpec[CurrentPic.Mode].Name);

  record = adif_build_sstv_record(CurrentPic.RemoteCall, station_call,
                                  qso_date, time_on, comment, &error);
  g_free(qso_date);
  g_free(time_on);
  g_free(station_call);
  g_free(comment);
  if (record == NULL) {
    gtk_statusbar_push(GTK_STATUSBAR(gui.statusbar), 0,
                       error != NULL ? error->message : "Unable to build ADIF record.");
    g_clear_error(&error);
    return;
  }

  if (!adif_udp_send(ADIF_UDP_HOST, ADIF_UDP_PORT, record, &error)) {
    gtk_statusbar_push(GTK_STATUSBAR(gui.statusbar), 0,
                       error != NULL ? error->message : "ADIF UDP send failed.");
    g_clear_error(&error);
    g_free(record);
    return;
  }
  status = g_strdup_printf("ADIF QSO datagram sent to %s:%u (UDP has no delivery acknowledgement).",
                           ADIF_UDP_HOST, ADIF_UDP_PORT);
  gtk_statusbar_push(GTK_STATUSBAR(gui.statusbar), 0, status);
  g_free(status);
  g_free(record);
}

void createGUI() {

  GtkBuilder *builder;
  GtkWidget  *notebook, *tx_page;

  builder = gtk_builder_new();
  gtk_builder_add_from_file(builder, "slowrx.ui",      NULL);
  gtk_builder_add_from_file(builder, "aboutdialog.ui", NULL);
  
  gui.button_abort    = GTK_WIDGET(gtk_builder_get_object(builder,"button_abort"));
  gui.button_adif     = GTK_WIDGET(gtk_builder_get_object(builder,"button_adif"));
  gui.button_answer   = GTK_WIDGET(gtk_builder_get_object(builder,"button_answer"));
  gui.button_browse   = GTK_WIDGET(gtk_builder_get_object(builder,"button_browse"));
  gui.button_clear    = GTK_WIDGET(gtk_builder_get_object(builder,"button_clear"));
  gui.button_start    = GTK_WIDGET(gtk_builder_get_object(builder,"button_start"));
  gui.combo_card      = GTK_WIDGET(gtk_builder_get_object(builder,"combo_card"));
  gui.combo_mode      = GTK_WIDGET(gtk_builder_get_object(builder,"combo_mode"));
  gui.entry_picdir    = GTK_WIDGET(gtk_builder_get_object(builder,"entry_picdir"));
  gui.eventbox_img    = GTK_WIDGET(gtk_builder_get_object(builder,"eventbox_img"));
  gui.frame_manual    = GTK_WIDGET(gtk_builder_get_object(builder,"frame_manual"));
  gui.frame_slant     = GTK_WIDGET(gtk_builder_get_object(builder,"frame_slant"));
  gui.grid_vu         = GTK_WIDGET(gtk_builder_get_object(builder,"grid_vu"));
  gui.iconview        = GTK_WIDGET(gtk_builder_get_object(builder,"SavedIconView"));
  gui.image_devstatus = GTK_WIDGET(gtk_builder_get_object(builder,"image_devstatus"));
  gui.image_pwr       = GTK_WIDGET(gtk_builder_get_object(builder,"image_pwr"));
  gui.image_rx        = GTK_WIDGET(gtk_builder_get_object(builder,"image_rx"));
  gui.image_snr       = GTK_WIDGET(gtk_builder_get_object(builder,"image_snr"));
  gui.label_fskid     = GTK_WIDGET(gtk_builder_get_object(builder,"label_fskid"));
  gui.label_lastmode  = GTK_WIDGET(gtk_builder_get_object(builder,"label_lastmode"));
  gui.label_utc       = GTK_WIDGET(gtk_builder_get_object(builder,"label_utc"));
  gui.menuitem_quit   = GTK_WIDGET(gtk_builder_get_object(builder,"menuitem_quit"));
  gui.menuitem_about  = GTK_WIDGET(gtk_builder_get_object(builder,"menuitem_about"));
  gui.spin_shift      = GTK_WIDGET(gtk_builder_get_object(builder,"spin_shift"));
  gui.statusbar       = GTK_WIDGET(gtk_builder_get_object(builder,"statusbar"));
  gui.tog_adapt       = GTK_WIDGET(gtk_builder_get_object(builder,"tog_adapt"));
  gui.tog_fsk         = GTK_WIDGET(gtk_builder_get_object(builder,"tog_fsk"));
  gui.tog_rx          = GTK_WIDGET(gtk_builder_get_object(builder,"tog_rx"));
  gui.tog_save        = GTK_WIDGET(gtk_builder_get_object(builder,"tog_save"));
  gui.tog_setedge     = GTK_WIDGET(gtk_builder_get_object(builder,"tog_setedge"));
  gui.tog_slant       = GTK_WIDGET(gtk_builder_get_object(builder,"tog_slant"));
  gui.window_about    = GTK_WIDGET(gtk_builder_get_object(builder,"window_about"));
  gui.window_main     = GTK_WIDGET(gtk_builder_get_object(builder,"window_main"));

  notebook = GTK_WIDGET(gtk_builder_get_object(builder,"notebook1"));
  if (gui.button_answer != NULL)
    gtk_widget_set_sensitive(gui.button_answer, FALSE);
  if (gui.button_adif != NULL)
    gtk_widget_set_sensitive(gui.button_adif, FALSE);
  tx_page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
  gtk_container_set_border_width(GTK_CONTAINER(tx_page), 12);
  gui.tx_panel = sstv_tx_panel_new(GTK_WINDOW(gui.window_main), config);
  gtk_box_pack_start(GTK_BOX(tx_page), gui.tx_panel, TRUE, TRUE, 0);
  // Embeds the SSTV transmit control panel directly in the TX tab
  gtk_notebook_insert_page(GTK_NOTEBOOK(notebook), tx_page,
                           gtk_label_new("TX"), 1);

  g_signal_connect        (gui.button_abort,  "clicked",      G_CALLBACK(evt_AbortRx),       NULL);
  if (gui.button_answer != NULL)
    g_signal_connect(gui.button_answer, "clicked", G_CALLBACK(evt_answer), notebook);
  if (gui.button_adif != NULL)
    g_signal_connect(gui.button_adif, "clicked", G_CALLBACK(evt_log_adif), NULL);
  g_signal_connect        (gui.button_browse, "clicked",      G_CALLBACK(evt_chooseDir),     NULL);
  g_signal_connect        (gui.button_clear,  "clicked",      G_CALLBACK(evt_clearPix),      NULL);
  g_signal_connect        (gui.button_start,  "clicked",      G_CALLBACK(evt_ManualStart),   NULL);
  g_signal_connect        (gui.combo_card,    "changed",      G_CALLBACK(evt_changeDevices), NULL);
  g_signal_connect        (gui.eventbox_img,  "button-press-event",G_CALLBACK(evt_clickimg),     NULL);
  g_signal_connect        (gui.menuitem_quit, "activate",     G_CALLBACK(evt_deletewindow),  NULL);
  g_signal_connect        (gui.menuitem_about,"activate",     G_CALLBACK(evt_show_about),    NULL);
  g_signal_connect_swapped(gui.tog_adapt,     "toggled",      G_CALLBACK(evt_GetAdaptive),   NULL);
  g_signal_connect        (gui.window_main,   "delete-event", G_CALLBACK(evt_deletewindow),  NULL);

  savedstore = GTK_LIST_STORE(gtk_icon_view_get_model(GTK_ICON_VIEW(gui.iconview)));

  pixbuf_rx   = gdk_pixbuf_new (GDK_COLORSPACE_RGB, FALSE, 8, 320, 256);
  gdk_pixbuf_fill(pixbuf_rx, 0x000000ff);
  pixbuf_disp = gdk_pixbuf_scale_simple (pixbuf_rx, 500, 400, GDK_INTERP_BILINEAR);
  gtk_image_set_from_pixbuf(GTK_IMAGE(gui.image_rx), pixbuf_disp);

  pixbuf_PWR = gdk_pixbuf_new (GDK_COLORSPACE_RGB, FALSE, 8, 100, 30);
  pixbuf_SNR = gdk_pixbuf_new (GDK_COLORSPACE_RGB, FALSE, 8, 100, 30);

  gtk_combo_box_set_active(GTK_COMBO_BOX(gui.combo_mode), 0);

  if (g_key_file_get_string(config,"slowrx","rxdir",NULL) != NULL) {
    gtk_entry_set_text(GTK_ENTRY(gui.entry_picdir),g_key_file_get_string(config,"slowrx","rxdir",NULL));
  } else {
    g_key_file_set_string(config,"slowrx","rxdir",g_get_home_dir());
    gtk_entry_set_text(GTK_ENTRY(gui.entry_picdir),g_key_file_get_string(config,"slowrx","rxdir",NULL));
  }

  //setVU(0, 6);

  gtk_widget_show_all  (gui.window_main);

}

// Draw signal level meters according to given values
void setVU (double *Power, int FFTLen, int WinIdx, gboolean ShowWin) {
  int          x,y, W=100, H=30;
  guchar       *pixelsPWR, *pixelsSNR, *pPWR, *pSNR;
  unsigned int rowstridePWR,rowstrideSNR, LoBin, HiBin, i;
  double       logpow,p;

  rowstridePWR = gdk_pixbuf_get_rowstride (pixbuf_PWR);
  pixelsPWR    = gdk_pixbuf_get_pixels    (pixbuf_PWR);
  
  rowstrideSNR = gdk_pixbuf_get_rowstride (pixbuf_SNR);
  pixelsSNR    = gdk_pixbuf_get_pixels    (pixbuf_SNR);

  for (y=0; y<H; y++) {
    for (x=0; x<W; x++) {

      pPWR = pixelsPWR + y * rowstridePWR + (W-1-x) * 3;
      pSNR = pixelsSNR + y * rowstrideSNR + (W-1-x) * 3;

      if ((5-WinIdx)  >= (W-1-x)/16) {
        if (y > 3 && y < H-3 && (W-1-x) % 16 >3 && x % 2 == 0) {
            pSNR[0] = 0x34;
            pSNR[1] = 0xe4;
            pSNR[2] = 0x84;
        } else {
          pSNR[0] = 0x20;
          pSNR[1] = 0x20;
          pSNR[2] = 0x20;
        }
      } else {
        if (y > 3 && y < H-3 && (W-1-x) % 16 >3 && x % 2 == 0) {
          pSNR[0] = pSNR[1] = pSNR[2] = 0x40;
        } else {
          pSNR[0] = pSNR[1] = pSNR[2] = 0x20;
        }
      }

      LoBin = (int)((W-1-x)*(6000/W)/44100.0 * FFTLen);
      HiBin = (int)((W  -x)*(6000/W)/44100.0 * FFTLen);

      logpow = 0;
      for (i=LoBin; i<HiBin; i++) logpow += log(850*Power[i]) / 2;

      p = (H-1-y)/(H/23.0);

      pPWR[0] = pPWR[1] = pPWR[2] = 0;

      if (logpow > p) {
        pPWR[0] = 0;//clip(pPWR[0] + 0x22 * (logpow-p) / (HiBin-LoBin+1));
        pPWR[1] = 192;//clip(pPWR[1] + 0x66 * (logpow-p) / (HiBin-LoBin+1));
        pPWR[2] = 64;//clip(pPWR[2] + 0x22 * (logpow-p) / (HiBin-LoBin+1));
      }

      /*if (ShowWin && LoBin >= GetBin(1200+CurrentPic.HedrShift, FFTLen) &&
          HiBin <= GetBin(2300+CurrentPic.HedrShift, FFTLen)) {
        pPWR[0] = clip(pPWR[0] + 0x66);
        pPWR[1] = clip(pPWR[1] + 0x11);
        pPWR[2] = clip(pPWR[2] + 0x11);
      }*/

    }
  }

  gdk_threads_enter();
  gtk_image_set_from_pixbuf(GTK_IMAGE(gui.image_pwr), pixbuf_PWR);
  gtk_image_set_from_pixbuf(GTK_IMAGE(gui.image_snr), pixbuf_SNR);
  gdk_threads_leave();

}

void evt_chooseDir() {
  GtkWidget *dialog;
  dialog = gtk_file_chooser_dialog_new ("Select folder",
                                      GTK_WINDOW(gui.window_main),
                                      GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER,
                                      GTK_STOCK_CANCEL, GTK_RESPONSE_CANCEL,
                                      GTK_STOCK_OPEN, GTK_RESPONSE_ACCEPT,
                                      NULL);
  
  if (gtk_dialog_run (GTK_DIALOG (dialog)) == GTK_RESPONSE_ACCEPT) {
    g_key_file_set_string(config,"slowrx","rxdir",gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog)));
    gtk_entry_set_text(GTK_ENTRY(gui.entry_picdir),gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog)));
  }

  gtk_widget_destroy (dialog);
}

void evt_show_about() {
  gtk_dialog_run(GTK_DIALOG(gui.window_about));
  gtk_widget_hide(gui.window_about);
}
