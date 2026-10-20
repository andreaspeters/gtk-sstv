#include "txdialog.h"

#include <errno.h>
#include <gtk/gtk.h>
#include <glib/gstdio.h>
#include <alsa/asoundlib.h>
#include <math.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <fftw3.h>

#include "common.h"
#include "compose.h"
#include "encoder.h"
#include "txaudio.h"

#define TX_STATE_KEY "slowrx-tx-dialog-state"

typedef struct {
  GtkWidget *panel;
  GtkWindow *parent;
  GtkWidget *source_combo;
  GtkWidget *preview;
  GtkWidget *my_call;
  GtkWidget *their_call;
  GtkWidget *report;
  GtkWidget *message;
  GtkWidget *message_presets;
  GtkWidget *station_info;
  GtkWidget *station_font_size;
  GtkWidget *station_color;
  GtkWidget *utc_label;
  GtkWidget *status;
  GtkWidget *send_button;
  GtkWidget *audio_device;
  GPtrArray *audio_device_ids;
  GtkWidget *fskid_toggle;
  GtkWidget *mode_combo;
  GtkWidget *reply_position;
  GdkPixbuf *received;
  GdkPixbuf *source;
  GKeyFile *config;
  gboolean has_received;
  gboolean changing_source;
  gboolean reply_mode;
  gboolean playing;
  gboolean destroyed;
} TxDialogState;

static gboolean compose_preview(TxDialogState *state, GdkPixbuf **composed_out);

typedef struct {
  GtkWidget *panel;
  gchar *device;
  SstvPcm pcm;
  gboolean success;
  gchar *message;
} TxPlaybackJob;

typedef struct {
  GtkWidget *panel;
  GtkWidget *check;
  GdkPixbuf *composed;
  SstvPcm pcm;
  gchar *my_call;
  gchar *audio_device;
  SstvMode mode;
  gboolean append_id;
} TxPendingTransmission;

static void tx_state_free(gpointer data) {
  TxDialogState *state = data;
  if (state->received != NULL) g_object_unref(state->received);
  if (state->source != NULL) g_object_unref(state->source);
  g_clear_pointer(&state->audio_device_ids, g_ptr_array_unref);
  g_free(state);
}

static const gchar *entry_text(GtkWidget *widget) {
  return gtk_editable_get_text(GTK_EDITABLE(widget));
}

static SstvMode selected_mode(TxDialogState *state) {
  guint selected = gtk_drop_down_get_selected(GTK_DROP_DOWN(state->mode_combo));
  SstvMode mode = SSTV_MODE_ROBOT36;
  if (selected == 1) mode = SSTV_MODE_MARTIN_M1;
  else if (selected == 2) mode = SSTV_MODE_MARTIN_M2;
  return mode;
}

static const gchar *selected_mode_id(TxDialogState *state) {
  guint selected = gtk_drop_down_get_selected(GTK_DROP_DOWN(state->mode_combo));
  if (selected == 1) return "MARTIN_M1";
  if (selected == 2) return "MARTIN_M2";
  return "ROBOT36";
}

static const gchar *selected_audio_device(TxDialogState *state) {
  guint selected = gtk_drop_down_get_selected(
      GTK_DROP_DOWN(state->audio_device));
  if (state->audio_device_ids == NULL ||
      selected >= state->audio_device_ids->len)
    return "default";
  return g_ptr_array_index(state->audio_device_ids, selected);
}

static void populate_audio_devices(TxDialogState *state,
                                   const gchar *saved_device) {
  GtkStringList *names;
  int card = -1;
  int status;
  guint selected = 0;

  names = GTK_STRING_LIST(gtk_drop_down_get_model(
      GTK_DROP_DOWN(state->audio_device)));
  state->audio_device_ids = g_ptr_array_new_with_free_func(g_free);
  g_ptr_array_add(state->audio_device_ids, g_strdup("default"));
  while ((status = snd_card_next(&card)) >= 0 && card != -1) {
    char *cardname = NULL;
    gchar *playback_device;

    if (snd_card_get_name(card, &cardname) < 0 || cardname == NULL) {
      free(cardname);
      continue;
    }

    playback_device = g_strdup_printf("plughw:%d", card);
    gtk_string_list_append(names, cardname);
    g_ptr_array_add(state->audio_device_ids, g_strdup(playback_device));
    if (g_strcmp0(saved_device, playback_device) == 0)
      selected = state->audio_device_ids->len - 1;
    g_free(playback_device);
    free(cardname);
  }

  if (g_strcmp0(saved_device, "default") == 0)
    selected = 0;
  gtk_drop_down_set_selected(GTK_DROP_DOWN(state->audio_device), selected);
}

static void callsign_changed(GtkEditable *editable, gpointer user_data) {
  TxDialogState *state = user_data;
  g_key_file_set_string(state->config, "slowrx", "tx_callsign",
                        gtk_editable_get_text(GTK_EDITABLE(editable)));
}

static void device_changed(GObject *object, GParamSpec *pspec,
                           gpointer user_data) {
  TxDialogState *state = user_data;
  (void)object;
  (void)pspec;
  g_key_file_set_string(state->config, "slowrx", "tx_device",
                        selected_audio_device(state));
}

static void fskid_toggled(GtkToggleButton *button, gpointer user_data) {
  TxDialogState *state = user_data;
  g_key_file_set_boolean(state->config, "slowrx", "tx_fskid",
                         gtk_toggle_button_get_active(button));
}

static const gchar *selected_reply_position_id(TxDialogState *state) {
  guint selected = gtk_drop_down_get_selected(
      GTK_DROP_DOWN(state->reply_position));
  return selected == 1 ? "right" : "left";
}

static void reply_position_changed(GObject *object, GParamSpec *pspec,
                                   gpointer user_data) {
  TxDialogState *state = user_data;
  (void)object;
  (void)pspec;
  g_key_file_set_string(state->config, "slowrx", "tx_reply_position",
                        selected_reply_position_id(state));
  if (state->reply_mode) compose_preview(state, NULL);
}

static void mode_changed(GObject *object, GParamSpec *pspec,
                         gpointer user_data) {
  TxDialogState *state = user_data;
  (void)object;
  (void)pspec;
  g_key_file_set_string(state->config, "slowrx", "tx_mode",
                        selected_mode_id(state));
}

static const gchar *message_text(TxDialogState *state) {
  return gtk_editable_get_text(GTK_EDITABLE(state->message));
}

static gchar *station_info_text(TxDialogState *state) {
  GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(state->station_info));
  GtkTextIter start, end;
  gtk_text_buffer_get_bounds(buffer, &start, &end);
  return gtk_text_buffer_get_text(buffer, &start, &end, FALSE);
}

static void message_changed(GtkEditable *editable, gpointer user_data) {
  TxDialogState *state = user_data;
  g_key_file_set_string(state->config, "slowrx", "tx_message",
                        gtk_editable_get_text(GTK_EDITABLE(editable)));
  if (state->source != NULL) compose_preview(state, NULL);
}

static void message_preset_changed(GObject *object, GParamSpec *pspec,
                                   gpointer user_data) {
  TxDialogState *state = user_data;
  guint selected = gtk_drop_down_get_selected(GTK_DROP_DOWN(object));
  GtkStringList *presets;
  const gchar *text;
  (void)pspec;
  if (selected == 0 || selected == GTK_INVALID_LIST_POSITION) return;
  presets = GTK_STRING_LIST(gtk_drop_down_get_model(GTK_DROP_DOWN(object)));
  text = gtk_string_list_get_string(presets, selected);
  if (text != NULL)
    gtk_editable_set_text(GTK_EDITABLE(state->message), text);
  gtk_drop_down_set_selected(GTK_DROP_DOWN(object), 0);
}

static void station_info_changed(GtkTextBuffer *buffer, gpointer user_data) {
  TxDialogState *state = user_data;
  GtkTextIter start, end;
  gchar *text;
  gtk_text_buffer_get_bounds(buffer, &start, &end);
  text = gtk_text_buffer_get_text(buffer, &start, &end, FALSE);
  g_key_file_set_string(state->config, "slowrx", "tx_station_info", text);
  g_free(text);
  if (state->source != NULL) compose_preview(state, NULL);
}

static void station_font_size_changed(GtkSpinButton *button, gpointer user_data) {
  TxDialogState *state = user_data;
  g_key_file_set_integer(state->config, "slowrx", "tx_station_font_size",
                         gtk_spin_button_get_value_as_int(button));
  if (state->source != NULL) compose_preview(state, NULL);
}

static void station_color_set(GObject *button, GParamSpec *pspec, gpointer user_data) {
  TxDialogState *state = user_data;
  const GdkRGBA *color;
  gchar *color_text;
  (void)pspec;
  color = gtk_color_dialog_button_get_rgba(GTK_COLOR_DIALOG_BUTTON(button));
  color_text = gdk_rgba_to_string(color);
  g_key_file_set_string(state->config, "slowrx", "tx_station_font_color", color_text);
  g_free(color_text);
  if (state->source != NULL) compose_preview(state, NULL);
}

static gchar *current_utc_text(void) {
  GDateTime *now = g_date_time_new_now_utc();
  gchar *text = g_date_time_format(now, "%Y-%m-%d %H:%M UTC");
  g_date_time_unref(now);
  return text;
}

static void set_status(TxDialogState *state, const gchar *text) {
  gtk_label_set_text(GTK_LABEL(state->status), text != NULL ? text : "");
}

static gboolean compose_preview(TxDialogState *state, GdkPixbuf **composed_out) {
  GError *error = NULL;
  gchar *utc_text;
  gchar *station_text;
  const gchar *position_id;
  GdkPixbuf *composed;
  GdkTexture *texture;
  GdkRGBA station_color;
  guint station_font_size;

  if (state->source == NULL) {
    set_status(state, "Choose a received picture or photo first.");
    return FALSE;
  }

  utc_text = current_utc_text();
  station_text = station_info_text(state);
  station_font_size = (guint)gtk_spin_button_get_value_as_int(
      GTK_SPIN_BUTTON(state->station_font_size));
  station_color = *gtk_color_dialog_button_get_rgba(
      GTK_COLOR_DIALOG_BUTTON(state->station_color));
  gtk_label_set_text(GTK_LABEL(state->utc_label), utc_text);
  if (state->reply_mode) {
    position_id = selected_reply_position_id(state);
    composed = sstv_compose_reply_image(state->source,
                                        entry_text(state->my_call),
                                        entry_text(state->their_call),
                                        entry_text(state->report),
                                        utc_text, message_text(state),
                                        station_text, station_font_size,
                                        &station_color,
                                        g_strcmp0(position_id, "right") == 0,
                                        &error);
  } else {
    composed = sstv_compose_image(state->source,
                                  entry_text(state->my_call),
                                  entry_text(state->their_call),
                                  entry_text(state->report),
                                  utc_text, message_text(state), station_text,
                                  station_font_size, &station_color, &error);
  }
  g_free(utc_text);
  g_free(station_text);
  if (composed == NULL) {
    set_status(state, error != NULL ? error->message : "Unable to compose image.");
    if (error != NULL) g_error_free(error);
    return FALSE;
  }
  texture = gui_texture_new_from_pixbuf(composed);
  gtk_picture_set_paintable(GTK_PICTURE(state->preview),
                            texture != NULL ? GDK_PAINTABLE(texture) : NULL);
  g_clear_object(&texture);
  if (composed_out != NULL) *composed_out = composed;
  else g_object_unref(composed);
  set_status(state, "Preview updated. Verify the callsigns and report before transmitting.");
  return TRUE;
}

static void reset_source_selection(TxDialogState *state) {
  state->changing_source = TRUE;
  gtk_drop_down_set_selected(GTK_DROP_DOWN(state->source_combo),
                             state->has_received ? 0 : GTK_INVALID_LIST_POSITION);
  state->changing_source = FALSE;
}

static void photo_chooser_response(GObject *source_object,
                                   GAsyncResult *result,
                                   gpointer user_data) {
  GtkWidget *panel = GTK_WIDGET(user_data);
  TxDialogState *state = g_object_get_data(G_OBJECT(panel), TX_STATE_KEY);
  GFile *file;
  gchar *filename = NULL;
  GdkPixbuf *loaded = NULL;
  GError *error = NULL;

  file = gtk_file_dialog_open_finish(GTK_FILE_DIALOG(source_object), result,
                                     NULL);
  if (state != NULL && !state->destroyed && file != NULL) {
    filename = g_file_get_path(file);
    if (filename != NULL)
      loaded = gdk_pixbuf_new_from_file(filename, &error);
    if (loaded == NULL) {
      set_status(state, error != NULL ? error->message :
                 "Unable to load the selected local image.");
      reset_source_selection(state);
    } else {
      if (state->source != NULL) g_object_unref(state->source);
      state->source = loaded;
      loaded = NULL;
      state->reply_mode = FALSE;
      set_status(state, "Photo loaded. Use Preview to check the composed SSTV image.");
      compose_preview(state, NULL);
    }
  } else if (state != NULL && !state->destroyed) {
    reset_source_selection(state);
  }
  g_clear_object(&file);
  g_clear_object(&loaded);
  g_clear_error(&error);
  g_free(filename);
  g_object_unref(panel);
}

static void add_pixbuf_format_mime_types(GtkFileFilter *filter) {
  GSList *formats = gdk_pixbuf_get_formats();
  GSList *format_item;

  for (format_item = formats; format_item != NULL;
       format_item = format_item->next) {
    GdkPixbufFormat *format = format_item->data;
    gchar **mime_types;
    gchar **mime_type;

    if (gdk_pixbuf_format_is_disabled(format))
      continue;
    mime_types = gdk_pixbuf_format_get_mime_types(format);
    if (mime_types == NULL)
      continue;
    for (mime_type = mime_types; *mime_type != NULL; ++mime_type)
      gtk_file_filter_add_mime_type(filter, *mime_type);
    g_strfreev(mime_types);
  }
  g_slist_free(formats);
}

static void choose_photo(TxDialogState *state) {
  GtkFileDialog *chooser;
  GListStore *filters;
  GtkFileFilter *filter;

  chooser = gtk_file_dialog_new();
  gtk_file_dialog_set_title(chooser, "Choose SSTV photo");
  gtk_file_dialog_set_accept_label(chooser, "_Open");
  gtk_file_dialog_set_modal(chooser, TRUE);
  filter = gtk_file_filter_new();
  gtk_file_filter_set_name(filter, "Image files");
  add_pixbuf_format_mime_types(filter);
  filters = g_list_store_new(GTK_TYPE_FILE_FILTER);
  g_list_store_append(filters, filter);
  gtk_file_dialog_set_filters(chooser, G_LIST_MODEL(filters));
  gtk_file_dialog_set_default_filter(chooser, filter);
  gtk_file_dialog_open(chooser, state->parent, NULL,
                       photo_chooser_response, g_object_ref(state->panel));
  g_object_unref(filters);
  g_object_unref(filter);
  g_object_unref(chooser);
}

static void source_changed(GObject *object, GParamSpec *pspec,
                           gpointer user_data) {
  TxDialogState *state = user_data;
  guint selected;
  (void)pspec;
  if (state->changing_source) return;
  selected = gtk_drop_down_get_selected(GTK_DROP_DOWN(object));
  if (selected == 1) {
    choose_photo(state);
  } else if (selected == 0 && state->has_received) {
    state->reply_mode = FALSE;
    if (state->source != NULL) g_object_unref(state->source);
    state->source = g_object_ref(state->received);
    set_status(state, "Using the most recently received SSTV image.");
    compose_preview(state, NULL);
  }
}

static gboolean playback_finished(gpointer data) {
  TxPlaybackJob *job = data;
  TxDialogState *state = g_object_get_data(G_OBJECT(job->panel), TX_STATE_KEY);
  if (state != NULL && !state->destroyed) {
    state->playing = FALSE;
    gtk_widget_set_sensitive(state->send_button, TRUE);
    set_status(state, job->success ?
               "SSTV audio playback finished." : job->message);
  }
  free(job->pcm.samples);
  g_free(job->message);
  g_free(job->device);
  g_object_unref(job->panel);
  g_free(job);
  return G_SOURCE_REMOVE;
}

static gpointer playback_thread(gpointer data) {
  TxPlaybackJob *job = data;
  GError *error = NULL;
  job->success = sstv_play_pcm(job->device, job->pcm.samples,
                               job->pcm.length, &error);
  job->message = error != NULL ? g_strdup(error->message) :
                                 g_strdup("SSTV audio playback failed.");
  if (error != NULL) g_error_free(error);
  g_idle_add(playback_finished, job);
  return NULL;
}

static gboolean append_fskid_audio(SstvPcm *image_pcm, const gchar *callsign,
                                   GError **error) {
  SstvPcm id_pcm = { NULL, 0 };
  gint16 *combined;
  size_t combined_length;

  if (!sstv_encode_fskid(callsign, &id_pcm, error)) return FALSE;
  if (id_pcm.length > G_MAXSIZE - image_pcm->length ||
      image_pcm->length + id_pcm.length > G_MAXSIZE / sizeof(*combined)) {
    free(id_pcm.samples);
    g_set_error_literal(error, g_quark_from_static_string("slowrx-sstv-tx"), 1,
                        "Combined SSTV and FSKID audio buffer is too large");
    return FALSE;
  }

  combined_length = image_pcm->length + id_pcm.length;
  combined = realloc(image_pcm->samples, combined_length * sizeof(*combined));
  if (combined == NULL) {
    free(id_pcm.samples);
    g_set_error_literal(error, g_quark_from_static_string("slowrx-sstv-tx"), 2,
                        "Unable to append FSKID audio to SSTV image");
    return FALSE;
  }
  memcpy(combined + image_pcm->length, id_pcm.samples,
         id_pcm.length * sizeof(*combined));
  free(id_pcm.samples);
  image_pcm->samples = combined;
  image_pcm->length = combined_length;
  return TRUE;
}

static gchar *filename_component(const gchar *text) {
  gchar *component = g_strdup(text != NULL ? text : "");
  gchar *p;
  for (p = component; *p != '\0'; ++p) {
    if (!g_ascii_isalnum((guchar)*p) && *p != '-' && *p != '_') *p = '_';
  }
  return component;
}

static gboolean save_transmit_image(GKeyFile *config, GdkPixbuf *image,
                                    SstvMode mode, const gchar *callsign,
                                    gchar **saved_path, GError **error) {
  gchar *rx_directory = NULL, *tx_directory = NULL;
  gchar *stamp = NULL, *mode_part = NULL, *call_part = NULL;
  gchar *filename = NULL, *path = NULL;
  GDateTime *now = NULL;
  GdkPixbuf *mode_image = NULL;
  int height = mode == SSTV_MODE_ROBOT36 ? 240 : 256;
  guint suffix = 0;
  gboolean saved = FALSE;

  if (saved_path != NULL) *saved_path = NULL;
  rx_directory = g_key_file_get_string(config, "slowrx", "rxdir", NULL);
  if (rx_directory == NULL || *rx_directory == '\0') {
    g_free(rx_directory);
    rx_directory = g_strdup(g_get_home_dir());
  }
  tx_directory = g_build_filename(rx_directory, "tx", NULL);
  if (g_mkdir_with_parents(tx_directory, 0755) != 0) {
    int saved_errno = errno;
    g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(saved_errno),
                "Cannot create TX log directory '%s': %s", tx_directory,
                g_strerror(saved_errno));
    goto cleanup;
  }

  mode_image = gdk_pixbuf_scale_simple(image, 320, height, GDK_INTERP_BILINEAR);
  if (mode_image == NULL) {
    g_set_error_literal(error, g_quark_from_static_string("slowrx-sstv-tx"), 3,
                        "Unable to create the mode-sized TX image for logging");
    goto cleanup;
  }

  now = g_date_time_new_now_utc();
  stamp = g_date_time_format(now, "%Y%m%d-%H%M%S");
  mode_part = filename_component(sstv_mode_name(mode));
  call_part = filename_component(callsign);
  do {
    g_free(filename);
    g_free(path);
    if (suffix == 0) {
      filename = g_strdup_printf("%s-%06d_%s_%s.png", stamp,
                                 g_date_time_get_microsecond(now),
                                 mode_part, call_part);
    } else {
      filename = g_strdup_printf("%s-%06d_%s_%s-%u.png", stamp,
                                 g_date_time_get_microsecond(now),
                                 mode_part, call_part, suffix);
    }
    path = g_build_filename(tx_directory, filename, NULL);
    ++suffix;
  } while (g_file_test(path, G_FILE_TEST_EXISTS));

  if (!gdk_pixbuf_save(mode_image, path, "png", error, NULL)) goto cleanup;
  if (saved_path != NULL) *saved_path = g_strdup(path);
  saved = TRUE;

cleanup:
  if (mode_image != NULL) g_object_unref(mode_image);
  if (now != NULL) g_date_time_unref(now);
  g_free(rx_directory);
  g_free(tx_directory);
  g_free(stamp);
  g_free(mode_part);
  g_free(call_part);
  g_free(filename);
  g_free(path);
  return saved;
}

static void tx_pending_transmission_free(gpointer data, GClosure *closure) {
  TxPendingTransmission *pending = data;
  (void)closure;
  if (pending->composed != NULL) g_object_unref(pending->composed);
  free(pending->pcm.samples);
  g_free(pending->my_call);
  g_free(pending->audio_device);
  g_object_unref(pending->panel);
  g_free(pending);
}

static void transmission_confirm_response(GtkWindow *dialog, gint response,
                                          gpointer user_data) {
  TxPendingTransmission *pending = user_data;
  TxDialogState *state = g_object_get_data(G_OBJECT(pending->panel), TX_STATE_KEY);
  TxPlaybackJob *job;
  GThread *thread;
  GError *error = NULL;
  gchar *saved_path = NULL;
  gchar *status_text;

  if (state == NULL || state->destroyed) {
    gtk_window_destroy(dialog);
    return;
  }
  if (response != GTK_RESPONSE_ACCEPT ||
      !gtk_check_button_get_active(GTK_CHECK_BUTTON(pending->check))) {
    set_status(state, "Transmission cancelled.");
    gtk_window_destroy(dialog);
    return;
  }
  if (!save_transmit_image(state->config, pending->composed, pending->mode,
                           pending->my_call, &saved_path, &error)) {
    set_status(state, error != NULL ? error->message : "Unable to save TX image.");
    g_clear_error(&error);
    gtk_window_destroy(dialog);
    return;
  }

  state->playing = TRUE;
  gtk_widget_set_sensitive(state->send_button, FALSE);
  status_text = g_strdup_printf(
      "Saved TX image: %s. Transmitting %s SSTV%s audio to '%s'...",
      saved_path, sstv_mode_name(pending->mode),
      pending->append_id ? " with FSKID after image" : "",
      pending->audio_device);
  set_status(state, status_text);
  g_free(status_text);
  g_free(saved_path);

  job = g_new0(TxPlaybackJob, 1);
  job->panel = g_object_ref(state->panel);
  job->device = g_strdup(pending->audio_device);
  job->pcm = pending->pcm;
  pending->pcm.samples = NULL;
  thread = g_thread_try_new("sstv-tx-audio", playback_thread, job, &error);
  if (thread == NULL) {
    state->playing = FALSE;
    gtk_widget_set_sensitive(state->send_button, TRUE);
    set_status(state, error != NULL ? error->message :
               "Unable to start playback thread.");
    g_clear_error(&error);
    free(job->pcm.samples);
    g_free(job->device);
    g_object_unref(job->panel);
    g_free(job);
    gtk_window_destroy(dialog);
    return;
  }
  g_thread_unref(thread);
  gtk_window_destroy(dialog);
}

static void tx_pending_transmission_destroy(gpointer data) {
  tx_pending_transmission_free(data, NULL);
}

static void transmission_confirm_button_clicked(GtkButton *button,
                                                gpointer user_data) {
  GtkWindow *dialog = GTK_WINDOW(user_data);
  TxPendingTransmission *pending =
      g_object_get_data(G_OBJECT(dialog), "tx-pending");
  gint response = GPOINTER_TO_INT(
      g_object_get_data(G_OBJECT(button), "tx-response"));

  if (pending != NULL)
    transmission_confirm_response(dialog, response, pending);
}

static gboolean transmission_confirm_close_request(GtkWindow *dialog,
                                                   gpointer user_data) {
  TxPendingTransmission *pending =
      g_object_get_data(G_OBJECT(dialog), "tx-pending");
  (void)user_data;

  if (pending == NULL)
    return FALSE;
  transmission_confirm_response(dialog, GTK_RESPONSE_DELETE_EVENT, pending);
  return TRUE;
}

static void begin_transmission(TxDialogState *state) {
  GdkPixbuf *composed = NULL;
  SstvPcm pcm = { NULL, 0 };
  GError *error = NULL;
  GtkWidget *confirm;
  GtkWidget *root_box;
  GtkWidget *message_box;
  GtkWidget *header_box;
  GtkWidget *text_box;
  GtkWidget *warning_icon;
  GtkWidget *heading;
  GtkWidget *body;
  GtkWidget *check;
  GtkWidget *action_box;
  GtkWidget *cancel_button;
  GtkWidget *transmit_button;
  TxPendingTransmission *pending;
  const gchar *my_call = entry_text(state->my_call);
  const gchar *audio_device;
  SstvMode mode;
  gchar *confirm_title;
  gchar *confirm_text;
  gboolean append_id;

  if (*my_call == '\0') {
    set_status(state, "Enter your own callsign before preparing the SSTV image.");
    gtk_widget_grab_focus(state->my_call);
    return;
  }
  audio_device = selected_audio_device(state);
  mode = selected_mode(state);
  append_id = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(state->fskid_toggle));
  if (!compose_preview(state, &composed)) return;
  if (!sstv_encode_mode(composed, mode, &pcm, &error)) {
    set_status(state, error != NULL ? error->message : "SSTV encoding failed.");
    g_clear_error(&error);
    g_object_unref(composed);
    return;
  }
  if (append_id && !append_fskid_audio(&pcm, my_call, &error)) {
    set_status(state, error != NULL ? error->message : "FSKID encoding failed.");
    g_clear_error(&error);
    free(pcm.samples);
    g_object_unref(composed);
    return;
  }

  confirm_title = g_strdup_printf("Ready to send a %s SSTV picture%s?",
                                  sstv_mode_name(mode),
                                  append_id ? " with FSKID after image" : "");
  confirm_text = g_strdup_printf(
      "This plays 44.1 kHz audio on '%s'. %s If that output is connected to a radio, use the matching SSTV mode and frequency and key PTT/VOX only when ready. slowrx does not control PTT.",
      audio_device != NULL ? audio_device : "default",
      append_id ? "Your FSKID callsign will follow the image." : "No FSKID will be sent.");
  confirm = gtk_window_new();
  gtk_window_set_application(GTK_WINDOW(confirm),
                             gtk_window_get_application(state->parent));
  gtk_window_set_title(GTK_WINDOW(confirm), "SSTV transmission confirmation");
  gtk_window_set_transient_for(GTK_WINDOW(confirm), state->parent);
  gtk_window_set_modal(GTK_WINDOW(confirm), TRUE);
  gtk_window_set_destroy_with_parent(GTK_WINDOW(confirm), TRUE);

  root_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
  gtk_widget_set_margin_top(root_box, 16);
  gtk_widget_set_margin_bottom(root_box, 16);
  gtk_widget_set_margin_start(root_box, 16);
  gtk_widget_set_margin_end(root_box, 16);
  message_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
  header_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
  warning_icon = gtk_image_new_from_icon_name("dialog-warning-symbolic");
  gtk_image_set_pixel_size(GTK_IMAGE(warning_icon), 32);
  gtk_widget_set_valign(warning_icon, GTK_ALIGN_START);
  text_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
  heading = gtk_label_new(confirm_title);
  gtk_label_set_xalign(GTK_LABEL(heading), 0.0);
  gtk_label_set_wrap(GTK_LABEL(heading), TRUE);
  gtk_widget_add_css_class(heading, "title-3");
  body = gtk_label_new(confirm_text);
  gtk_label_set_xalign(GTK_LABEL(body), 0.0);
  gtk_label_set_wrap(GTK_LABEL(body), TRUE);
  gtk_label_set_selectable(GTK_LABEL(body), TRUE);
  gtk_box_append(GTK_BOX(text_box), heading);
  gtk_box_append(GTK_BOX(text_box), body);
  gtk_box_append(GTK_BOX(header_box), warning_icon);
  gtk_box_append(GTK_BOX(header_box), text_box);
  gtk_box_append(GTK_BOX(message_box), header_box);
  gtk_widget_set_visible(warning_icon, TRUE);
  gtk_widget_set_visible(heading, TRUE);
  gtk_widget_set_visible(body, TRUE);
  gtk_widget_set_visible(text_box, TRUE);
  gtk_widget_set_visible(header_box, TRUE);

  g_free(confirm_text);
  g_free(confirm_title);
  check = gtk_check_button_new_with_label(
      "I have checked the radio, frequency and transmit path");
  gtk_widget_set_margin_top(check, 6);
  gtk_widget_set_margin_bottom(check, 6);
  gtk_widget_set_margin_start(check, 6);
  gtk_widget_set_margin_end(check, 6);
  gtk_box_append(GTK_BOX(message_box), check);
  gtk_widget_set_visible(check, TRUE);
  gtk_widget_set_visible(message_box, TRUE);

  action_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_widget_set_halign(action_box, GTK_ALIGN_END);
  cancel_button = gtk_button_new_with_label("Cancel");
  transmit_button = gtk_button_new_with_label("Transmit audio");
  gtk_box_append(GTK_BOX(action_box), cancel_button);
  gtk_box_append(GTK_BOX(action_box), transmit_button);
  gtk_box_append(GTK_BOX(root_box), message_box);
  gtk_box_append(GTK_BOX(root_box), action_box);
  gtk_window_set_child(GTK_WINDOW(confirm), root_box);
  gtk_window_set_default_widget(GTK_WINDOW(confirm), transmit_button);
  gtk_widget_set_visible(cancel_button, TRUE);
  gtk_widget_set_visible(transmit_button, TRUE);
  gtk_widget_set_visible(action_box, TRUE);
  gtk_widget_set_visible(root_box, TRUE);

  pending = g_new0(TxPendingTransmission, 1);
  pending->panel = g_object_ref(state->panel);
  pending->check = check;
  pending->composed = composed;
  pending->pcm = pcm;
  pending->my_call = g_strdup(my_call);
  pending->audio_device = g_strdup(audio_device != NULL ? audio_device : "default");
  pending->mode = mode;
  pending->append_id = append_id;
  g_object_set_data_full(G_OBJECT(confirm), "tx-pending", pending,
                         tx_pending_transmission_destroy);
  g_signal_connect(confirm, "close-request",
                   G_CALLBACK(transmission_confirm_close_request), NULL);
  g_object_set_data(G_OBJECT(cancel_button), "tx-response",
                    GINT_TO_POINTER(GTK_RESPONSE_CANCEL));
  g_object_set_data(G_OBJECT(transmit_button), "tx-response",
                    GINT_TO_POINTER(GTK_RESPONSE_ACCEPT));
  g_signal_connect(cancel_button, "clicked",
                   G_CALLBACK(transmission_confirm_button_clicked), confirm);
  g_signal_connect(transmit_button, "clicked",
                   G_CALLBACK(transmission_confirm_button_clicked), confirm);
  gtk_window_present(GTK_WINDOW(confirm));
}

static void panel_destroyed(GtkWidget *widget, gpointer user_data) {
  TxDialogState *state = user_data;
  (void)widget;
  state->destroyed = TRUE;
}

static void send_clicked(GtkButton *button, gpointer user_data) {
  TxDialogState *state = user_data;
  (void)button;
  if (!state->playing) begin_transmission(state);
}

static void preview_clicked(GtkButton *button, gpointer user_data) {
  TxDialogState *state = user_data;
  (void)button;
  compose_preview(state, NULL);
}

GtkWidget *sstv_tx_panel_new(GtkBuilder *builder, GtkWindow *parent,
                             GKeyFile *config) {
  TxDialogState *state;
  GtkWidget *preview_button;
  gchar *stored_call, *stored_device, *stored_mode, *stored_reply_position;
  gchar *stored_message, *stored_station_info, *stored_font_color;
  gint stored_font_size;
  GdkRGBA station_color;

  state = g_new0(TxDialogState, 1);
  state->parent = parent;
  state->config = config;
  state->panel = GTK_WIDGET(gtk_builder_get_object(builder, "tx_panel"));
  state->source_combo = GTK_WIDGET(gtk_builder_get_object(builder, "tx_source_combo"));
  state->audio_device = GTK_WIDGET(gtk_builder_get_object(builder, "tx_audio_device"));
  state->mode_combo = GTK_WIDGET(gtk_builder_get_object(builder, "tx_mode_combo"));
  state->reply_position = GTK_WIDGET(gtk_builder_get_object(builder, "tx_reply_position"));
  state->my_call = GTK_WIDGET(gtk_builder_get_object(builder, "tx_my_call"));
  state->fskid_toggle = GTK_WIDGET(gtk_builder_get_object(builder, "tx_fskid_toggle"));
  state->their_call = GTK_WIDGET(gtk_builder_get_object(builder, "tx_their_call"));
  state->report = GTK_WIDGET(gtk_builder_get_object(builder, "tx_report"));
  state->message = GTK_WIDGET(gtk_builder_get_object(builder, "tx_message"));
  state->message_presets = GTK_WIDGET(gtk_builder_get_object(builder, "tx_message_presets"));
  state->station_info = GTK_WIDGET(gtk_builder_get_object(builder, "tx_station_info"));
  state->station_font_size = GTK_WIDGET(gtk_builder_get_object(builder, "tx_station_font_size"));
  state->station_color = GTK_WIDGET(gtk_builder_get_object(builder, "tx_station_color"));
  state->utc_label = GTK_WIDGET(gtk_builder_get_object(builder, "tx_utc_label"));
  state->preview = GTK_WIDGET(gtk_builder_get_object(builder, "tx_preview"));
  state->status = GTK_WIDGET(gtk_builder_get_object(builder, "tx_status"));
  state->send_button = GTK_WIDGET(gtk_builder_get_object(builder, "tx_send_button"));
  preview_button = GTK_WIDGET(gtk_builder_get_object(builder, "tx_preview_button"));
  if (state->panel == NULL || state->source_combo == NULL ||
      state->audio_device == NULL || state->mode_combo == NULL ||
      state->reply_position == NULL || state->my_call == NULL ||
      state->fskid_toggle == NULL || state->their_call == NULL ||
      state->report == NULL || state->message == NULL ||
      state->message_presets == NULL || state->station_info == NULL ||
      state->station_font_size == NULL || state->station_color == NULL ||
      state->utc_label == NULL || state->preview == NULL ||
      state->status == NULL || state->send_button == NULL ||
      preview_button == NULL) {
    g_warning("The GtkBuilder TX page is missing a required widget");
    g_free(state);
    return NULL;
  }

  /* Keep the user-editable TX controls editable even if a Builder default
   * or a parent/container state changes independently. */
  gtk_editable_set_editable(GTK_EDITABLE(state->my_call), TRUE);
  gtk_editable_set_editable(GTK_EDITABLE(state->their_call), TRUE);
  gtk_editable_set_editable(GTK_EDITABLE(state->report), TRUE);
  gtk_editable_set_editable(GTK_EDITABLE(state->message), TRUE);
  gtk_text_view_set_editable(GTK_TEXT_VIEW(state->station_info), TRUE);
  gtk_widget_set_focusable(state->my_call, TRUE);
  gtk_widget_set_focusable(state->their_call, TRUE);
  gtk_widget_set_focusable(state->report, TRUE);
  gtk_widget_set_focusable(state->message, TRUE);
  gtk_widget_set_focusable(state->station_info, TRUE);
  gtk_widget_set_focus_on_click(state->my_call, TRUE);
  gtk_widget_set_focus_on_click(state->their_call, TRUE);
  gtk_widget_set_focus_on_click(state->report, TRUE);
  gtk_widget_set_focus_on_click(state->message, TRUE);
  gtk_widget_set_focus_on_click(state->station_info, TRUE);
  gtk_widget_set_sensitive(state->my_call, TRUE);
  gtk_widget_set_sensitive(state->their_call, TRUE);
  gtk_widget_set_sensitive(state->report, TRUE);
  gtk_widget_set_sensitive(state->message, TRUE);
  gtk_widget_set_sensitive(state->station_info, TRUE);

  g_object_set_data_full(G_OBJECT(state->panel), TX_STATE_KEY, state, tx_state_free);
  g_signal_connect(state->panel, "destroy", G_CALLBACK(panel_destroyed), state);

  /* Prevent the initial model selection from opening the file chooser. */
  gtk_drop_down_set_selected(GTK_DROP_DOWN(state->source_combo),
                             GTK_INVALID_LIST_POSITION);
  g_signal_connect(state->source_combo, "notify::selected",
                   G_CALLBACK(source_changed), state);

  stored_device = g_key_file_get_string(config, "slowrx", "tx_device", NULL);
  populate_audio_devices(state, stored_device);
  g_free(stored_device);
  g_signal_connect(state->audio_device, "notify::selected",
                   G_CALLBACK(device_changed), state);

  stored_mode = g_key_file_get_string(config, "slowrx", "tx_mode", NULL);
  if (g_strcmp0(stored_mode, "MARTIN_M1") == 0)
    gtk_drop_down_set_selected(GTK_DROP_DOWN(state->mode_combo), 1);
  else if (g_strcmp0(stored_mode, "MARTIN_M2") == 0)
    gtk_drop_down_set_selected(GTK_DROP_DOWN(state->mode_combo), 2);
  else
    gtk_drop_down_set_selected(GTK_DROP_DOWN(state->mode_combo), 0);
  g_free(stored_mode);
  g_signal_connect(state->mode_combo, "notify::selected",
                   G_CALLBACK(mode_changed), state);

  stored_reply_position = g_key_file_get_string(config, "slowrx", "tx_reply_position", NULL);
  gtk_drop_down_set_selected(GTK_DROP_DOWN(state->reply_position),
                             g_strcmp0(stored_reply_position, "right") == 0 ? 1 : 0);
  g_free(stored_reply_position);
  g_signal_connect(state->reply_position, "notify::selected",
                   G_CALLBACK(reply_position_changed), state);

  stored_call = g_key_file_get_string(config, "slowrx", "tx_callsign", NULL);
  if (stored_call != NULL)
    gtk_editable_set_text(GTK_EDITABLE(state->my_call), stored_call);
  g_free(stored_call);
  g_signal_connect(state->my_call, "changed", G_CALLBACK(callsign_changed), state);

  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(state->fskid_toggle),
                               g_key_file_get_boolean(config, "slowrx", "tx_fskid", NULL));
  g_signal_connect(state->fskid_toggle, "toggled", G_CALLBACK(fskid_toggled), state);

  stored_message = g_key_file_get_string(config, "slowrx", "tx_message", NULL);
  gtk_editable_set_text(GTK_EDITABLE(state->message),
                        stored_message != NULL ? stored_message : "TNX FER QSO");
  g_free(stored_message);
  g_signal_connect(state->message, "changed", G_CALLBACK(message_changed), state);
  g_signal_connect(state->message_presets, "notify::selected",
                   G_CALLBACK(message_preset_changed), state);

  stored_station_info = g_key_file_get_string(config, "slowrx", "tx_station_info", NULL);
  gtk_text_buffer_set_text(
      gtk_text_view_get_buffer(GTK_TEXT_VIEW(state->station_info)),
      stored_station_info != NULL ? stored_station_info : "", -1);
  g_free(stored_station_info);
  g_signal_connect(gtk_text_view_get_buffer(GTK_TEXT_VIEW(state->station_info)),
                   "changed", G_CALLBACK(station_info_changed), state);

  stored_font_size = g_key_file_get_integer(config, "slowrx", "tx_station_font_size", NULL);
  if (stored_font_size < 8 || stored_font_size > 28) stored_font_size = 14;
  gtk_spin_button_set_value(GTK_SPIN_BUTTON(state->station_font_size), stored_font_size);

  stored_font_color = g_key_file_get_string(config, "slowrx", "tx_station_font_color", NULL);
  if (stored_font_color == NULL || !gdk_rgba_parse(&station_color, stored_font_color))
    gdk_rgba_parse(&station_color, "#ffffff");
  g_free(stored_font_color);
  gtk_color_dialog_button_set_rgba(GTK_COLOR_DIALOG_BUTTON(state->station_color),
                                   &station_color);
  g_signal_connect(state->station_font_size, "value-changed",
                   G_CALLBACK(station_font_size_changed), state);
  g_signal_connect(state->station_color, "notify::rgba",
                   G_CALLBACK(station_color_set), state);

  g_signal_connect(preview_button, "clicked", G_CALLBACK(preview_clicked), state);
  g_signal_connect(state->send_button, "clicked", G_CALLBACK(send_clicked), state);

  return state->panel;
}

static void update_received(TxDialogState *state, GdkPixbuf *received,
                            const char *received_call, double snr_db,
                            gboolean snr_valid, gboolean prepare_reply) {
  GdkPixbuf *copy;
  gchar *report;
  guint source_index;
  gboolean use_received;

  if (state == NULL || received == NULL || state->destroyed) return;
  copy = gdk_pixbuf_copy(received);
  if (copy == NULL) return;

  source_index = gtk_drop_down_get_selected(GTK_DROP_DOWN(state->source_combo));
  use_received = prepare_reply || source_index != 1 || state->source == NULL;
  if (use_received && state->source != NULL) {
    g_object_unref(state->source);
    state->source = NULL;
  }
  if (state->received != NULL) g_object_unref(state->received);
  state->received = copy;
  state->has_received = TRUE;
  if (use_received) state->source = g_object_ref(state->received);

  gtk_editable_set_text(GTK_EDITABLE(state->their_call),
                        received_call != NULL ? received_call : "");
  report = (snr_valid && isfinite(snr_db))
             ? g_strdup_printf("SNR %+.1f dB", snr_db) : g_strdup("");
  gtk_editable_set_text(GTK_EDITABLE(state->report), report);
  g_free(report);

  if (prepare_reply) {
    gchar *configured_message;
    state->reply_mode = TRUE;
    configured_message = g_key_file_get_string(state->config, "slowrx",
                                                "tx_message", NULL);
    gtk_editable_set_text(GTK_EDITABLE(state->message),
                          configured_message != NULL ? configured_message
                                                     : "TNX FER SSTV QSO 73");
    g_free(configured_message);
  }

  if (use_received) {
    state->changing_source = TRUE;
    gtk_drop_down_set_selected(GTK_DROP_DOWN(state->source_combo), 0);
    state->changing_source = FALSE;
  }
  compose_preview(state, NULL);
  if (prepare_reply)
    set_status(state, "Reply image ready. Review the preview and transmit when ready.");
}

void sstv_tx_panel_set_received(GtkWidget *panel, GdkPixbuf *received,
                                const char *received_call, double snr_db,
                                gboolean snr_valid) {
  TxDialogState *state;
  if (panel == NULL || received == NULL) return;
  state = g_object_get_data(G_OBJECT(panel), TX_STATE_KEY);
  update_received(state, received, received_call, snr_db, snr_valid, FALSE);
}

void sstv_tx_panel_prepare_reply(GtkWidget *panel, GdkPixbuf *received,
                                 const char *received_call, double snr_db,
                                 gboolean snr_valid) {
  TxDialogState *state;
  if (panel == NULL || received == NULL) return;
  state = g_object_get_data(G_OBJECT(panel), TX_STATE_KEY);
  update_received(state, received, received_call, snr_db, snr_valid, TRUE);
}
