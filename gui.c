#include <stdlib.h>
#include <string.h>
#include <gtk/gtk.h>
#include <alsa/asoundlib.h>
#include <math.h>

#include <fftw3.h>

#include "common.h"
#include "txdialog.h"
#include "adifudp.h"

#define ADIF_UDP_HOST "127.0.0.1"
#define ADIF_UDP_PORT 2333
#define WATERFALL_WIDTH 512
#define WATERFALL_HEIGHT 120
#define WATERFALL_MAX_HZ 5000.0
#define WATERFALL_REFRESH_SAMPLES 4410

typedef struct {
  guint8 pixels[WATERFALL_WIDTH * 3];
  guchar mode;
  gshort shift;
  gchar guide_text[192];
} WaterfallRow;

static GdkPixbuf *waterfall_pixbuf;
static double waterfall_hann[1024];
static guint64 waterfall_sample_count;

static void draw_waterfall(GtkDrawingArea *area, cairo_t *cr,
                           int width, int height, gpointer user_data) {
  int image_width, image_height;

  (void)area;
  (void)user_data;
  if (waterfall_pixbuf == NULL || width <= 0 || height <= 0) return;
  image_width = gdk_pixbuf_get_width(waterfall_pixbuf);
  image_height = gdk_pixbuf_get_height(waterfall_pixbuf);
  if (image_width <= 0 || image_height <= 0) return;

  cairo_save(cr);
  cairo_scale(cr, width / (double)image_width, height / (double)image_height);
  gdk_cairo_set_source_pixbuf(cr, waterfall_pixbuf, 0, 0);
  cairo_paint(cr);
  cairo_restore(cr);
}

static guint frequency_guide_x(double frequency, double minimum,
                               double maximum, guint width) {
  if (width == 0 || frequency < minimum || frequency > maximum ||
      maximum <= minimum)
    return G_MAXUINT;
  return (guint)floor((frequency - minimum) * (width - 1) /
                      (maximum - minimum) + 0.5);
}

static void draw_frequency_guide_column(guchar *pixels, guint width,
                                        guint height, guint rowstride,
                                        guint x, guint8 red, guint8 green,
                                        guint8 blue) {
  guint y;
  if (x >= width) return;
  for (y = 0; y < height; ++y) {
    guchar *pixel = pixels + y * rowstride + x * 3;
    pixel[0] = red;
    pixel[1] = green;
    pixel[2] = blue;
  }
}

static void draw_frequency_guides(guchar *pixels, guint width, guint height,
                                  guint rowstride, double minimum,
                                  double maximum, gint shift) {
  static const double targets[] = {1200.0, 1500.0, 2300.0};
  guint i;
  if (pixels == NULL || width == 0 || height == 0 || maximum <= minimum)
    return;

  if (shift != 0) {
    for (i = 0; i < G_N_ELEMENTS(targets); ++i) {
      guint x = frequency_guide_x(targets[i] + shift, minimum, maximum, width);
      draw_frequency_guide_column(pixels, width, height, rowstride, x,
                                  255, 0, 255);
    }
  }

  draw_frequency_guide_column(
      pixels, width, height, rowstride,
      frequency_guide_x(targets[0], minimum, maximum, width), 0, 225, 255);
  for (i = 1; i < G_N_ELEMENTS(targets); ++i) {
    guint x = frequency_guide_x(targets[i], minimum, maximum, width);
    draw_frequency_guide_column(pixels, width, height, rowstride, x,
                                255, 210, 0);
  }
}

static void format_frequency_guide(gchar *text, gsize text_size, guchar mode,
                                   gshort shift) {
  const gchar *mode_name = "Auto";
  if (mode > UNKNOWN && mode <= W2180 && ModeSpec[mode].ShortName != NULL)
    mode_name = ModeSpec[mode].ShortName;
  g_snprintf(text, text_size,
             "SSTV %s · sync 1200 Hz · image 1500–2300 Hz · audio offset %+d Hz · radio tune: USB %+d / LSB %+d Hz",
             mode_name, (gint)shift, (gint)shift, -(gint)shift);
}

static void evt_answer(GtkButton *button, gpointer user_data) {
  GtkNotebook *notebook = GTK_NOTEBOOK(user_data);
  (void)button;

  if (!CurrentPic.ReceivedValid || pixbuf_rx == NULL || gui.tx_panel == NULL)
    return;

  sstv_tx_panel_prepare_reply(gui.tx_panel, pixbuf_rx, CurrentPic.RemoteCall,
                              CurrentPic.SnrDb, CurrentPic.SnrValid);
  gtk_notebook_set_current_page(notebook, 1);
}

static void cache_widget_option(GtkWidget *widget, gpointer user_data) {
  GuiOption option = GPOINTER_TO_INT(user_data);

  if (option == GUI_OPTION_MODE_INDEX) {
    gui_option_set_integer(option,
        (gint)gtk_drop_down_get_selected(GTK_DROP_DOWN(widget)));
  } else if (option == GUI_OPTION_SHIFT) {
    gui_option_set_integer(option,
        gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(widget)));
  } else {
    gui_option_set_toggle(option,
        gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(widget)));
  }
}

static void cache_dropdown_option(GObject *object, GParamSpec *pspec,
                                  gpointer user_data) {
  (void)pspec;
  cache_widget_option(GTK_WIDGET(object), user_data);
}

static void app_quit_action(GSimpleAction *action, GVariant *parameter,
                            gpointer user_data) {
  (void)action;
  (void)parameter;
  (void)user_data;
  evt_deletewindow(NULL, NULL);
}

static void app_about_action(GSimpleAction *action, GVariant *parameter,
                             gpointer user_data) {
  (void)action;
  (void)parameter;
  (void)user_data;
  evt_show_about(NULL, NULL);
}

static void add_application_action(GtkApplication *application,
                                   const gchar *name, GCallback callback) {
  GSimpleAction *action = g_simple_action_new(name, NULL);
  g_signal_connect(action, "activate", callback, NULL);
  g_action_map_add_action(G_ACTION_MAP(application), G_ACTION(action));
  g_object_unref(action);
}

static void saved_picture_setup(GtkSignalListItemFactory *factory,
                                GtkListItem *list_item,
                                gpointer user_data) {
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
  GtkWidget *image = gtk_picture_new();
  GtkWidget *label = gtk_label_new(NULL);
  (void)factory;
  (void)user_data;

  gtk_widget_set_halign(image, GTK_ALIGN_CENTER);
  gtk_widget_set_halign(label, GTK_ALIGN_CENTER);
  gtk_picture_set_can_shrink(GTK_PICTURE(image), FALSE);
  gtk_picture_set_content_fit(GTK_PICTURE(image), GTK_CONTENT_FIT_CONTAIN);
  gtk_box_append(GTK_BOX(box), image);
  gtk_box_append(GTK_BOX(box), label);
  gtk_list_item_set_child(list_item, box);
}

static void saved_picture_bind(GtkSignalListItemFactory *factory,
                               GtkListItem *list_item,
                               gpointer user_data) {
  GtkWidget *box = gtk_list_item_get_child(list_item);
  GtkWidget *image = gtk_widget_get_first_child(box);
  GtkWidget *label = gtk_widget_get_next_sibling(image);
  GtkStringObject *picture = GTK_STRING_OBJECT(
      gtk_list_item_get_item(list_item));
  GdkTexture *texture = g_object_get_data(G_OBJECT(picture),
                                          "slowrx-thumbnail");
  const gchar *id = gtk_string_object_get_string(picture);
  (void)factory;
  (void)user_data;

  gtk_picture_set_paintable(GTK_PICTURE(image),
                            texture != NULL ? GDK_PAINTABLE(texture) : NULL);
  gtk_label_set_text(GTK_LABEL(label), id != NULL ? id : "");
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
    gtk_label_set_text(GTK_LABEL(gui.statusbar),
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
    gtk_label_set_text(GTK_LABEL(gui.statusbar),
                       error != NULL ? error->message : "Unable to build ADIF record.");
    g_clear_error(&error);
    return;
  }

  if (!adif_udp_send(ADIF_UDP_HOST, ADIF_UDP_PORT, record, &error)) {
    gtk_label_set_text(GTK_LABEL(gui.statusbar),
                       error != NULL ? error->message : "ADIF UDP send failed.");
    g_clear_error(&error);
    g_free(record);
    return;
  }
  status = g_strdup_printf("ADIF QSO datagram sent to %s:%u (UDP has no delivery acknowledgement).",
                           ADIF_UDP_HOST, ADIF_UDP_PORT);
  gtk_label_set_text(GTK_LABEL(gui.statusbar), status);
  g_free(status);
  g_free(record);
}

static gboolean evt_close_main_window(GtkWindow *window, gpointer user_data) {
  GApplication *application = g_application_get_default();
  (void)window;
  (void)user_data;
  g_atomic_int_set(&ApplicationStopping, TRUE);
  Abort = TRUE;
  if (application != NULL)
    g_application_quit(application);
  return FALSE;
}

typedef struct {
  GtkWidget *image;
  GdkPixbuf *pixbuf;
} ImageUpdate;

static gboolean apply_image_update(gpointer data) {
  ImageUpdate *update = data;
  if (GTK_IS_PICTURE(update->image))
    gui_picture_set_pixbuf(GTK_PICTURE(update->image), update->pixbuf);
  else if (GTK_IS_IMAGE(update->image))
    gui_image_set_pixbuf(GTK_IMAGE(update->image), update->pixbuf);
  g_object_unref(update->image);
  g_object_unref(update->pixbuf);
  g_free(update);
  return G_SOURCE_REMOVE;
}

void gui_set_image_pixbuf(GtkWidget *image, GdkPixbuf *pixbuf) {
  ImageUpdate *update;
  if (image == NULL || pixbuf == NULL)
    return;
  update = g_new(ImageUpdate, 1);
  update->image = g_object_ref(image);
  update->pixbuf = g_object_ref(pixbuf);
  g_main_context_invoke(NULL, apply_image_update, update);
}

static gboolean append_waterfall_row(gpointer user_data) {
  WaterfallRow *row = user_data;
  gint rowstride;
  guchar *pixels;

  if (waterfall_pixbuf == NULL || g_atomic_int_get(&ApplicationStopping))
    return G_SOURCE_REMOVE;

  rowstride = gdk_pixbuf_get_rowstride(waterfall_pixbuf);
  pixels = gdk_pixbuf_get_pixels(waterfall_pixbuf);
  memmove(pixels, pixels + rowstride,
          (WATERFALL_HEIGHT - 1) * (gsize)rowstride);
  memcpy(pixels + (WATERFALL_HEIGHT - 1) * rowstride, row->pixels,
         sizeof(row->pixels));
  if (gui.label_waterfall != NULL)
    gtk_label_set_text(GTK_LABEL(gui.label_waterfall), row->guide_text);
  if (gui.image_waterfall != NULL)
    gtk_widget_set_tooltip_text(gui.image_waterfall, row->guide_text);
  if (gui.image_pwr != NULL)
    gtk_widget_set_tooltip_text(gui.image_pwr, row->guide_text);
  if (gui.image_waterfall != NULL)
    gtk_widget_queue_draw(gui.image_waterfall);
  return G_SOURCE_REMOVE;
}

void gui_waterfall_init(void) {
  guint i;

  waterfall_pixbuf = gdk_pixbuf_new(GDK_COLORSPACE_RGB, FALSE, 8,
                                    WATERFALL_WIDTH, WATERFALL_HEIGHT);
  if (waterfall_pixbuf == NULL) return;
  gdk_pixbuf_fill(waterfall_pixbuf, 0x080c14ff);
  for (i = 0; i < G_N_ELEMENTS(waterfall_hann); ++i)
    waterfall_hann[i] = 0.5 * (1.0 - cos(2.0 * G_PI * i / 1023.0));
  if (gui.image_waterfall != NULL)
    gtk_widget_queue_draw(gui.image_waterfall);
}

void gui_waterfall_update_samples(guint samples_read) {
  WaterfallRow *row;
  guint i, x;
  guchar *pixels;

  if (samples_read == 0 || waterfall_pixbuf == NULL || pcm.Buffer == NULL ||
      fft.Plan1024 == NULL || g_atomic_int_get(&ApplicationStopping))
    return;

  waterfall_sample_count += samples_read;
  if (waterfall_sample_count < WATERFALL_REFRESH_SAMPLES) return;
  waterfall_sample_count %= WATERFALL_REFRESH_SAMPLES;
  if (pcm.WindowPtr < 512 || pcm.WindowPtr > BUFLEN - 512) return;

  for (i = 0; i < 1024; ++i)
    fft.in[i] = pcm.Buffer[pcm.WindowPtr + (gint)i - 512] / 32768.0 *
                waterfall_hann[i];
  fftw_execute(fft.Plan1024);

  row = g_new(WaterfallRow, 1);
  pixels = row->pixels;
  row->mode = (guchar)(gui_option_get_integer(GUI_OPTION_MODE_INDEX) + 1);
  if (row->mode > W2180) row->mode = UNKNOWN;
  row->shift = CurrentPic.HedrShift;
  for (x = 0; x < WATERFALL_WIDTH; ++x) {
    double frequency = 100.0 + (WATERFALL_MAX_HZ - 100.0) * x /
                               (WATERFALL_WIDTH - 1.0);
    guint bin = (guint)(frequency * 1024.0 / 44100.0);
    double normalized_power = power(fft.out[bin]) / (256.0 * 256.0);
    double db = normalized_power > 1e-12
        ? 10.0 * log10(normalized_power) : -120.0;
    double level = (db + 85.0) / 85.0;
    guint8 red, green, blue;

    level = fmax(0.0, fmin(1.0, level));
    if (level < 0.25) {
      red = 0;
      green = 0;
      blue = (guint8)(16.0 + level * 4.0 * 112.0);
    } else if (level < 0.5) {
      red = 0;
      green = (guint8)((level - 0.25) * 4.0 * 255.0);
      blue = (guint8)(128.0 + (level - 0.25) * 4.0 * 127.0);
    } else if (level < 0.75) {
      red = (guint8)((level - 0.5) * 4.0 * 255.0);
      green = 255;
      blue = (guint8)(255.0 - (level - 0.5) * 4.0 * 255.0);
    } else {
      red = 255;
      green = (guint8)(255.0 - (level - 0.75) * 4.0 * 255.0);
      blue = 0;
    }
    pixels[3 * x] = red;
    pixels[3 * x + 1] = green;
    pixels[3 * x + 2] = blue;
  }

  draw_frequency_guides(pixels, WATERFALL_WIDTH, 1,
                        WATERFALL_WIDTH * 3, 100.0,
                        WATERFALL_MAX_HZ, row->shift);
  format_frequency_guide(row->guide_text, sizeof(row->guide_text),
                         row->mode, row->shift);

  g_main_context_invoke_full(NULL, G_PRIORITY_DEFAULT, append_waterfall_row,
                             row, g_free);
}

void gui_waterfall_shutdown(void) {
  g_clear_object(&waterfall_pixbuf);
  waterfall_sample_count = 0;
}

void createGUI(GtkApplication *application) {

  GtkBuilder *builder;
  GdkPixbuf *app_logo;
  GdkTexture *app_logo_texture;
  GError *icon_error = NULL;
  GtkWidget  *notebook;
  GtkGesture *image_click;
  GtkNoSelection *saved_selection;
  GtkListItemFactory *saved_factory;

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
  gui.saved_view      = GTK_WIDGET(gtk_builder_get_object(builder,"SavedGridView"));
  gui.image_devstatus = GTK_WIDGET(gtk_builder_get_object(builder,"image_devstatus"));
  gui.image_pwr       = GTK_WIDGET(gtk_builder_get_object(builder,"image_pwr"));
  gui.image_rx        = GTK_WIDGET(gtk_builder_get_object(builder,"image_rx"));
  gui.image_snr       = GTK_WIDGET(gtk_builder_get_object(builder,"image_snr"));
  gui.image_waterfall = GTK_WIDGET(gtk_builder_get_object(builder,"image_waterfall"));
  if (GTK_IS_DRAWING_AREA(gui.image_waterfall))
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(gui.image_waterfall),
                                   draw_waterfall, NULL, NULL);
  gui.label_fskid     = GTK_WIDGET(gtk_builder_get_object(builder,"label_fskid"));
  gui.label_lastmode  = GTK_WIDGET(gtk_builder_get_object(builder,"label_lastmode"));
  gui.label_waterfall = GTK_WIDGET(gtk_builder_get_object(builder,"label_waterfall"));
  gui.label_utc       = GTK_WIDGET(gtk_builder_get_object(builder,"label_utc"));

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

  app_logo = gdk_pixbuf_new_from_resource("/org/sstv/sstv.png", &icon_error);
  if (app_logo != NULL) {
    app_logo_texture = gui_texture_new_from_pixbuf(app_logo);
    if (app_logo_texture != NULL) {
      gtk_about_dialog_set_logo(GTK_ABOUT_DIALOG(gui.window_about),
                                GDK_PAINTABLE(app_logo_texture));
      g_object_unref(app_logo_texture);
    }
    g_object_unref(app_logo);
  } else {
    g_warning("Unable to load application icon resource: %s",
              icon_error != NULL ? icon_error->message : "unknown error");
    g_clear_error(&icon_error);
  }

  gtk_window_set_application(GTK_WINDOW(gui.window_main), application);
  gtk_window_set_application(GTK_WINDOW(gui.window_about), application);
  add_application_action(application, "quit", G_CALLBACK(app_quit_action));
  add_application_action(application, "about", G_CALLBACK(app_about_action));
  {
    const gchar *quit_accels[] = {"<Primary>q", NULL};
    gtk_application_set_accels_for_action(application, "app.quit", quit_accels);
  }

  notebook = GTK_WIDGET(gtk_builder_get_object(builder,"notebook1"));
  if (gui.button_answer != NULL)
    gtk_widget_set_sensitive(gui.button_answer, FALSE);
  if (gui.button_adif != NULL)
    gtk_widget_set_sensitive(gui.button_adif, FALSE);

  {
    GtkWidget *main_box = GTK_WIDGET(gtk_builder_get_object(builder, "MainBox"));
    GtkWidget *alignment = GTK_WIDGET(gtk_builder_get_object(builder, "alignment1"));
    GtkWidget *menubar = GTK_WIDGET(gtk_builder_get_object(builder, "menubar1"));
    GtkWidget *new_main_box;
    GtkWidget *new_alignment;

    if (GTK_IS_BOX(main_box) && GTK_IS_BOX(alignment) &&
        GTK_IS_NOTEBOOK(notebook) && menubar != NULL &&
        gtk_widget_get_parent(alignment) == main_box &&
        gtk_widget_get_parent(notebook) == alignment &&
        gtk_widget_get_parent(menubar) == main_box) {
      /* Rebuild the two outer boxes in C: the Builder-created wrappers prevent
       * GTK from assigning focus to editable controls in the notebook page. */
      new_main_box = gtk_box_new(
          gtk_orientable_get_orientation(GTK_ORIENTABLE(main_box)),
          gtk_box_get_spacing(GTK_BOX(main_box)));
      gtk_box_set_homogeneous(GTK_BOX(new_main_box),
                              gtk_box_get_homogeneous(GTK_BOX(main_box)));
      gtk_widget_set_name(new_main_box, gtk_widget_get_name(main_box));
      gtk_widget_set_margin_start(new_main_box, gtk_widget_get_margin_start(main_box));
      gtk_widget_set_margin_end(new_main_box, gtk_widget_get_margin_end(main_box));
      gtk_widget_set_margin_top(new_main_box, gtk_widget_get_margin_top(main_box));
      gtk_widget_set_margin_bottom(new_main_box, gtk_widget_get_margin_bottom(main_box));
      gtk_widget_set_halign(new_main_box, gtk_widget_get_halign(main_box));
      gtk_widget_set_valign(new_main_box, gtk_widget_get_valign(main_box));
      gtk_widget_set_hexpand(new_main_box, gtk_widget_get_hexpand(main_box));
      gtk_widget_set_vexpand(new_main_box, gtk_widget_get_vexpand(main_box));
      gtk_widget_set_sensitive(new_main_box, gtk_widget_get_sensitive(main_box));
      gtk_widget_set_focusable(new_main_box, gtk_widget_get_focusable(main_box));
      gtk_widget_set_visible(new_main_box, gtk_widget_get_visible(main_box));

      new_alignment = gtk_box_new(
          gtk_orientable_get_orientation(GTK_ORIENTABLE(alignment)),
          gtk_box_get_spacing(GTK_BOX(alignment)));
      gtk_box_set_homogeneous(GTK_BOX(new_alignment),
                              gtk_box_get_homogeneous(GTK_BOX(alignment)));
      gtk_widget_set_name(new_alignment, gtk_widget_get_name(alignment));
      gtk_widget_set_margin_start(new_alignment, gtk_widget_get_margin_start(alignment));
      gtk_widget_set_margin_end(new_alignment, gtk_widget_get_margin_end(alignment));
      gtk_widget_set_margin_top(new_alignment, gtk_widget_get_margin_top(alignment));
      gtk_widget_set_margin_bottom(new_alignment, gtk_widget_get_margin_bottom(alignment));
      gtk_widget_set_halign(new_alignment, gtk_widget_get_halign(alignment));
      gtk_widget_set_valign(new_alignment, gtk_widget_get_valign(alignment));
      gtk_widget_set_hexpand(new_alignment, gtk_widget_get_hexpand(alignment));
      gtk_widget_set_vexpand(new_alignment, gtk_widget_get_vexpand(alignment));
      gtk_widget_set_sensitive(new_alignment, gtk_widget_get_sensitive(alignment));
      gtk_widget_set_focusable(new_alignment, gtk_widget_get_focusable(alignment));
      gtk_widget_set_visible(new_alignment, gtk_widget_get_visible(alignment));

      g_object_ref(notebook);
      g_object_ref(menubar);
      gtk_box_remove(GTK_BOX(alignment), notebook);
      gtk_box_remove(GTK_BOX(main_box), menubar);
      gtk_box_remove(GTK_BOX(main_box), alignment);
      gtk_box_append(GTK_BOX(new_alignment), notebook);
      gtk_box_append(GTK_BOX(new_main_box), menubar);
      gtk_box_append(GTK_BOX(new_main_box), new_alignment);
      gtk_window_set_child(GTK_WINDOW(gui.window_main), new_main_box);
      g_object_unref(notebook);
      g_object_unref(menubar);
    } else {
      g_warning("Could not rebuild the GtkBuilder window containers for focus");
    }
  }

  gui.tx_panel = sstv_tx_panel_new(builder, GTK_WINDOW(gui.window_main), config);

  g_signal_connect        (gui.button_abort,  "clicked",      G_CALLBACK(evt_AbortRx),       NULL);
  if (gui.button_answer != NULL)
    g_signal_connect(gui.button_answer, "clicked", G_CALLBACK(evt_answer), notebook);
  if (gui.button_adif != NULL)
    g_signal_connect(gui.button_adif, "clicked", G_CALLBACK(evt_log_adif), NULL);
  g_signal_connect        (gui.button_browse, "clicked",      G_CALLBACK(evt_chooseDir),     NULL);
  g_signal_connect        (gui.button_clear,  "clicked",      G_CALLBACK(evt_clearPix),      NULL);
  g_signal_connect        (gui.button_start,  "clicked",      G_CALLBACK(evt_ManualStart),   NULL);
  g_signal_connect        (gui.combo_card,    "notify::selected",
                           G_CALLBACK(evt_changeDevices), NULL);
  image_click = gtk_gesture_click_new();
  gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(image_click), GDK_BUTTON_PRIMARY);
  gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(image_click), GTK_PHASE_CAPTURE);
  gtk_widget_add_controller(gui.eventbox_img, GTK_EVENT_CONTROLLER(image_click));
  g_signal_connect(image_click, "pressed", G_CALLBACK(evt_clickimg), NULL);

  g_signal_connect_swapped(gui.tog_adapt,     "toggled",      G_CALLBACK(evt_GetAdaptive),   NULL);
  g_signal_connect(gui.tog_rx, "toggled", G_CALLBACK(cache_widget_option),
                   GINT_TO_POINTER(GUI_OPTION_RECEIVE));
  g_signal_connect(gui.tog_fsk, "toggled", G_CALLBACK(cache_widget_option),
                   GINT_TO_POINTER(GUI_OPTION_FSK));
  g_signal_connect(gui.tog_slant, "toggled", G_CALLBACK(cache_widget_option),
                   GINT_TO_POINTER(GUI_OPTION_SLANT));
  g_signal_connect(gui.tog_save, "toggled", G_CALLBACK(cache_widget_option),
                   GINT_TO_POINTER(GUI_OPTION_SAVE));
  g_signal_connect(gui.tog_adapt, "toggled", G_CALLBACK(cache_widget_option),
                   GINT_TO_POINTER(GUI_OPTION_ADAPTIVE));
  g_signal_connect(gui.combo_mode, "notify::selected",
                   G_CALLBACK(cache_dropdown_option),
                   GINT_TO_POINTER(GUI_OPTION_MODE_INDEX));
  g_signal_connect(gui.spin_shift, "value-changed", G_CALLBACK(cache_widget_option),
                   GINT_TO_POINTER(GUI_OPTION_SHIFT));
  g_signal_connect        (gui.window_main,   "close-request", G_CALLBACK(evt_close_main_window), NULL);

  savedstore = g_list_store_new(GTK_TYPE_STRING_OBJECT);
  saved_selection = gtk_no_selection_new(G_LIST_MODEL(savedstore));
  saved_factory = gtk_signal_list_item_factory_new();
  g_signal_connect(saved_factory, "setup", G_CALLBACK(saved_picture_setup), NULL);
  g_signal_connect(saved_factory, "bind", G_CALLBACK(saved_picture_bind), NULL);
  gtk_grid_view_set_factory(GTK_GRID_VIEW(gui.saved_view),
                            GTK_LIST_ITEM_FACTORY(saved_factory));
  gtk_grid_view_set_model(GTK_GRID_VIEW(gui.saved_view),
                          GTK_SELECTION_MODEL(saved_selection));
  g_object_unref(saved_factory);
  g_object_unref(saved_selection);

  pixbuf_rx   = gdk_pixbuf_new (GDK_COLORSPACE_RGB, FALSE, 8, 320, 256);
  gdk_pixbuf_fill(pixbuf_rx, 0x000000ff);
  pixbuf_disp = gdk_pixbuf_scale_simple (pixbuf_rx, 500, 400, GDK_INTERP_BILINEAR);
  gui_picture_set_pixbuf(GTK_PICTURE(gui.image_rx), pixbuf_disp);

  pixbuf_PWR = gdk_pixbuf_new (GDK_COLORSPACE_RGB, FALSE, 8, 100, 30);
  pixbuf_SNR = gdk_pixbuf_new (GDK_COLORSPACE_RGB, FALSE, 8, 100, 30);
  gui_waterfall_init();

  gtk_drop_down_set_selected(GTK_DROP_DOWN(gui.combo_mode), 0);
  cache_widget_option(gui.tog_rx, GINT_TO_POINTER(GUI_OPTION_RECEIVE));
  cache_widget_option(gui.tog_fsk, GINT_TO_POINTER(GUI_OPTION_FSK));
  cache_widget_option(gui.tog_slant, GINT_TO_POINTER(GUI_OPTION_SLANT));
  cache_widget_option(gui.tog_save, GINT_TO_POINTER(GUI_OPTION_SAVE));
  cache_widget_option(gui.tog_adapt, GINT_TO_POINTER(GUI_OPTION_ADAPTIVE));
  cache_widget_option(gui.combo_mode, GINT_TO_POINTER(GUI_OPTION_MODE_INDEX));
  cache_widget_option(gui.spin_shift, GINT_TO_POINTER(GUI_OPTION_SHIFT));

  if (g_key_file_get_string(config,"slowrx","rxdir",NULL) != NULL) {
    gtk_editable_set_text(GTK_EDITABLE(gui.entry_picdir),g_key_file_get_string(config,"slowrx","rxdir",NULL));
  } else {
    g_key_file_set_string(config,"slowrx","rxdir",g_get_home_dir());
    gtk_editable_set_text(GTK_EDITABLE(gui.entry_picdir),g_key_file_get_string(config,"slowrx","rxdir",NULL));
  }

  //setVU(0, 6);

  gtk_window_present(GTK_WINDOW(gui.window_main));

}

// Draw signal level meters according to given values
void setVU (double *Power, int FFTLen, int WinIdx, gboolean ShowWin) {
  int          x,y, W=100, H=30;
  guchar       *pixelsPWR, *pixelsSNR, *pPWR, *pSNR;
  unsigned int rowstridePWR,rowstrideSNR, LoBin, HiBin, i;
  double       logpow,p;
  GdkPixbuf   *power_pixbuf, *snr_pixbuf;

  power_pixbuf = gdk_pixbuf_new(GDK_COLORSPACE_RGB, FALSE, 8, W, H);
  snr_pixbuf = gdk_pixbuf_new(GDK_COLORSPACE_RGB, FALSE, 8, W, H);
  if (power_pixbuf == NULL || snr_pixbuf == NULL) {
    g_clear_object(&power_pixbuf);
    g_clear_object(&snr_pixbuf);
    return;
  }
  rowstridePWR = gdk_pixbuf_get_rowstride(power_pixbuf);
  pixelsPWR = gdk_pixbuf_get_pixels(power_pixbuf);
  
  rowstrideSNR = gdk_pixbuf_get_rowstride(snr_pixbuf);
  pixelsSNR = gdk_pixbuf_get_pixels(snr_pixbuf);

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

  draw_frequency_guides(pixelsPWR, W, H, rowstridePWR, 0.0, 6000.0,
                        CurrentPic.HedrShift);

  gui_set_image_pixbuf(gui.image_pwr, power_pixbuf);
  gui_set_image_pixbuf(gui.image_snr, snr_pixbuf);
  g_object_unref(power_pixbuf);
  g_object_unref(snr_pixbuf);

}

static void evt_choose_dir_response(GObject *source_object,
                                    GAsyncResult *result,
                                    gpointer user_data) {
  GFile *folder;
  gchar *path;
  (void)user_data;

  folder = gtk_file_dialog_select_folder_finish(GTK_FILE_DIALOG(source_object),
                                                result, NULL);
  if (folder == NULL) return;
  path = g_file_get_path(folder);
  if (path != NULL) {
    g_key_file_set_string(config, "slowrx", "rxdir", path);
    gtk_editable_set_text(GTK_EDITABLE(gui.entry_picdir), path);
    g_free(path);
  }
  g_object_unref(folder);
}

void evt_chooseDir(GtkButton *button, gpointer user_data) {
  GtkFileDialog *dialog;
  (void)button;
  (void)user_data;

  dialog = gtk_file_dialog_new();
  gtk_file_dialog_set_title(dialog, "Select folder");
  gtk_file_dialog_set_accept_label(dialog, "Open");
  gtk_file_dialog_set_modal(dialog, TRUE);
  gtk_file_dialog_select_folder(dialog, GTK_WINDOW(gui.window_main), NULL,
                                evt_choose_dir_response, NULL);
  g_object_unref(dialog);
}

void evt_show_about(GtkWidget *item, gpointer user_data) {
  (void)item;
  (void)user_data;
  gtk_window_present(GTK_WINDOW(gui.window_about));
}
