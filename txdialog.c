#include "txdialog.h"

#include <gtk/gtk.h>
#include <alsa/asoundlib.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

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
  GtkWidget *utc_label;
  GtkWidget *status;
  GtkWidget *send_button;
  GtkWidget *audio_device;
  GtkWidget *mode_combo;
  GdkPixbuf *received;
  GdkPixbuf *source;
  GKeyFile *config;
  gboolean has_received;
  gboolean changing_source;
  gboolean playing;
  gboolean destroyed;
} TxDialogState;

typedef struct {
  GtkWidget *panel;
  gchar *device;
  SstvPcm pcm;
  gboolean success;
  gchar *message;
} TxPlaybackJob;

static void tx_state_free(gpointer data) {
  TxDialogState *state = data;
  if (state->received != NULL) g_object_unref(state->received);
  if (state->source != NULL) g_object_unref(state->source);
  g_free(state);
}

static const gchar *entry_text(GtkWidget *widget) {
  return gtk_entry_get_text(GTK_ENTRY(widget));
}

static SstvMode selected_mode(TxDialogState *state) {
  const gchar *id = gtk_combo_box_get_active_id(GTK_COMBO_BOX(state->mode_combo));
  SstvMode mode = SSTV_MODE_ROBOT36;
  if (g_strcmp0(id, "MARTIN_M1") == 0) mode = SSTV_MODE_MARTIN_M1;
  else if (g_strcmp0(id, "MARTIN_M2") == 0) mode = SSTV_MODE_MARTIN_M2;
  return mode;
}

static void populate_audio_devices(GtkWidget *combo, const gchar *saved_device) {
  int card = -1;
  int status;

  gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(combo), "default", "default");
  while ((status = snd_card_next(&card)) >= 0 && card != -1) {
    char *cardname = NULL;
    gchar *playback_device;

    if (snd_card_get_name(card, &cardname) < 0 || cardname == NULL) {
      free(cardname);
      continue;
    }

    playback_device = g_strdup_printf("plughw:%d", card);
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(combo), playback_device, cardname);
    g_free(playback_device);
    free(cardname);
  }

  if (saved_device == NULL ||
      !gtk_combo_box_set_active_id(GTK_COMBO_BOX(combo), saved_device))
    gtk_combo_box_set_active_id(GTK_COMBO_BOX(combo), "default");
}

static void callsign_changed(GtkEditable *editable, gpointer user_data) {
  TxDialogState *state = user_data;
  g_key_file_set_string(state->config, "slowrx", "tx_callsign",
                        gtk_entry_get_text(GTK_ENTRY(editable)));
}

static void device_changed(GtkComboBox *combo, gpointer user_data) {
  TxDialogState *state = user_data;
  const gchar *id = gtk_combo_box_get_active_id(combo);
  if (id != NULL) {
    g_key_file_set_string(state->config, "slowrx", "tx_device", id);
  }
}

static void mode_changed(GtkComboBox *combo, gpointer user_data) {
  TxDialogState *state = user_data;
  const gchar *id = gtk_combo_box_get_active_id(combo);
  if (id != NULL) {
    g_key_file_set_string(state->config, "slowrx", "tx_mode", id);
  }
}

static const gchar *message_text(TxDialogState *state) {
  GtkWidget *entry = gtk_bin_get_child(GTK_BIN(state->message));
  return (entry != NULL) ? gtk_entry_get_text(GTK_ENTRY(entry)) : "";
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
  GdkPixbuf *composed;

  if (state->source == NULL) {
    set_status(state, "Choose a received picture or photo first.");
    return FALSE;
  }

  utc_text = current_utc_text();
  gtk_label_set_text(GTK_LABEL(state->utc_label), utc_text);
  composed = sstv_compose_image(state->source,
                                entry_text(state->my_call),
                                entry_text(state->their_call),
                                entry_text(state->report),
                                utc_text, message_text(state), &error);
  g_free(utc_text);
  if (composed == NULL) {
    set_status(state, error != NULL ? error->message : "Unable to compose image.");
    if (error != NULL) g_error_free(error);
    return FALSE;
  }
  gtk_image_set_from_pixbuf(GTK_IMAGE(state->preview), composed);
  if (composed_out != NULL) *composed_out = composed;
  else g_object_unref(composed);
  set_status(state, "Preview updated. Verify the callsigns and report before transmitting.");
  return TRUE;
}

static void choose_photo(TxDialogState *state) {
  GtkWidget *chooser;
  GtkFileFilter *filter;
  gchar *filename;
  GError *error = NULL;
  GdkPixbuf *loaded;

  chooser = gtk_file_chooser_dialog_new("Choose SSTV photo",
                                       state->parent,
                                       GTK_FILE_CHOOSER_ACTION_OPEN,
                                       "_Cancel", GTK_RESPONSE_CANCEL,
                                       "_Open", GTK_RESPONSE_ACCEPT,
                                       NULL);
  filter = gtk_file_filter_new();
  gtk_file_filter_set_name(filter, "Image files");
  gtk_file_filter_add_pixbuf_formats(filter);
  gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(chooser), filter);

  if (gtk_dialog_run(GTK_DIALOG(chooser)) != GTK_RESPONSE_ACCEPT) {
    gtk_widget_destroy(chooser);
    state->changing_source = TRUE;
    gtk_combo_box_set_active(GTK_COMBO_BOX(state->source_combo),
                             state->has_received ? 0 : -1);
    state->changing_source = FALSE;
    return;
  }

  filename = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(chooser));
  gtk_widget_destroy(chooser);
  if (filename == NULL) return;
  loaded = gdk_pixbuf_new_from_file(filename, &error);
  g_free(filename);
  if (loaded == NULL) {
    set_status(state, error != NULL ? error->message : "Unable to load selected image.");
    if (error != NULL) g_error_free(error);
    state->changing_source = TRUE;
    gtk_combo_box_set_active(GTK_COMBO_BOX(state->source_combo),
                             state->has_received ? 0 : -1);
    state->changing_source = FALSE;
    return;
  }

  if (state->source != NULL) g_object_unref(state->source);
  state->source = loaded;
  set_status(state, "Photo loaded. Use Preview to check the composed SSTV image.");
  compose_preview(state, NULL);
}

static void source_changed(GtkComboBox *combo, gpointer user_data) {
  TxDialogState *state = user_data;
  int selected;
  if (state->changing_source) return;
  selected = gtk_combo_box_get_active(combo);
  if (selected == 1) {
    choose_photo(state);
  } else if (selected == 0 && state->has_received) {
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

static void begin_transmission(TxDialogState *state) {
  GdkPixbuf *composed = NULL;
  SstvPcm pcm = { NULL, 0 };
  GError *error = NULL;
  GtkWidget *confirm;
  GtkWidget *content;
  GtkWidget *check;
  TxPlaybackJob *job;
  GThread *thread;
  const gchar *my_call = entry_text(state->my_call);
  const gchar *audio_device;
  SstvMode mode;
  gchar *confirm_title;
  gchar *confirm_text;
  gchar *status_text;

  if (*my_call == '\0') {
    set_status(state, "Enter your own callsign before preparing the SSTV image.");
    gtk_widget_grab_focus(state->my_call);
    return;
  }
  audio_device = gtk_combo_box_get_active_id(GTK_COMBO_BOX(state->audio_device));
  mode = selected_mode(state);
  if (!compose_preview(state, &composed)) {
    return;
  }
  if (!sstv_encode_mode(composed, mode, &pcm, &error)) {
    set_status(state, error != NULL ? error->message : "SSTV encoding failed.");
    if (error != NULL) g_error_free(error);
    g_object_unref(composed);
    return;
  }
  g_object_unref(composed);

  confirm_title = g_strdup_printf("Ready to send a %s SSTV picture?",
                                  sstv_mode_name(mode));
  confirm = gtk_message_dialog_new(state->parent, GTK_DIALOG_MODAL,
                                   GTK_MESSAGE_WARNING, GTK_BUTTONS_NONE,
                                   "%s", confirm_title);
  g_free(confirm_title);
  confirm_text = g_strdup_printf("This plays 44.1 kHz audio on '%s'. If that output is connected to a radio, use the matching SSTV mode and frequency and key PTT/VOX only when ready. slowrx does not control PTT.",
                                 audio_device != NULL ? audio_device : "default");
  gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(confirm), "%s",
                                           confirm_text);
  g_free(confirm_text);
  content = gtk_message_dialog_get_message_area(GTK_MESSAGE_DIALOG(confirm));
  check = gtk_check_button_new_with_label("I have checked the radio, frequency and transmit path");
  gtk_box_pack_start(GTK_BOX(content), check, FALSE, FALSE, 6);
  gtk_dialog_add_button(GTK_DIALOG(confirm), "Cancel", GTK_RESPONSE_CANCEL);
  gtk_dialog_add_button(GTK_DIALOG(confirm), "Transmit audio", GTK_RESPONSE_ACCEPT);
  gtk_widget_show(check);
  if (gtk_dialog_run(GTK_DIALOG(confirm)) != GTK_RESPONSE_ACCEPT ||
      !gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(check))) {
    gtk_widget_destroy(confirm);
    free(pcm.samples);
    set_status(state, "Transmission cancelled.");
    return;
  }
  gtk_widget_destroy(confirm);

  state->playing = TRUE;
  gtk_widget_set_sensitive(state->send_button, FALSE);
  status_text = g_strdup_printf("Transmitting %s SSTV audio to '%s'...",
                                sstv_mode_name(mode),
                                audio_device != NULL ? audio_device : "default");
  set_status(state, status_text);
  g_free(status_text);

  job = g_new0(TxPlaybackJob, 1);
  job->panel = g_object_ref(state->panel);
  job->device = g_strdup(audio_device != NULL ? audio_device : "default");
  job->pcm = pcm;
  thread = g_thread_try_new("sstv-tx-audio", playback_thread, job, &error);
  if (thread == NULL) {
    state->playing = FALSE;
    gtk_widget_set_sensitive(state->send_button, TRUE);
    set_status(state, error != NULL ? error->message : "Unable to start playback thread.");
    if (error != NULL) g_error_free(error);
    free(job->pcm.samples);
    g_free(job->device);
    g_object_unref(job->panel);
    g_free(job);
    return;
  }
  g_thread_unref(thread);
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

static GtkWidget *add_labeled_row(GtkWidget *grid, const gchar *label,
                                  GtkWidget *widget, int row) {
  GtkWidget *caption = gtk_label_new(label);
  gtk_widget_set_halign(caption, GTK_ALIGN_START);
  gtk_grid_attach(GTK_GRID(grid), caption, 0, row, 1, 1);
  gtk_grid_attach(GTK_GRID(grid), widget, 1, row, 1, 1);
  return widget;
}

GtkWidget *sstv_tx_panel_new(GtkWindow *parent, GKeyFile *config) {
  TxDialogState *state;
  GtkWidget *grid, *preview_box, *button, *utc_caption;
  GtkWidget *message_entry;
  gchar *stored_call, *stored_device, *stored_mode;
  int row = 0;

  state = g_new0(TxDialogState, 1);
  state->parent = parent;
  state->config = config;
  state->panel = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
  g_object_set_data_full(G_OBJECT(state->panel), TX_STATE_KEY, state, tx_state_free);
  g_signal_connect(state->panel, "destroy", G_CALLBACK(panel_destroyed), state);

  grid = gtk_grid_new();
  gtk_grid_set_row_spacing(GTK_GRID(grid), 8);
  gtk_grid_set_column_spacing(GTK_GRID(grid), 10);
  gtk_container_set_border_width(GTK_CONTAINER(grid), 10);
  gtk_box_pack_start(GTK_BOX(state->panel), grid, TRUE, TRUE, 0);

  state->source_combo = gtk_combo_box_text_new();
  gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(state->source_combo),
                                 "Most recently received SSTV image");
  gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(state->source_combo),
                                 "Choose a photo file...");
  add_labeled_row(grid, "Image source", state->source_combo, row++);
  g_signal_connect(state->source_combo, "changed", G_CALLBACK(source_changed), state);

  state->audio_device = gtk_combo_box_text_new();
  stored_device = g_key_file_get_string(config, "slowrx", "tx_device", NULL);
  populate_audio_devices(state->audio_device, stored_device);
  g_free(stored_device);
  add_labeled_row(grid, "Transmit audio device", state->audio_device, row++);
  g_signal_connect(state->audio_device, "changed", G_CALLBACK(device_changed), state);

  state->mode_combo = gtk_combo_box_text_new();
  gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(state->mode_combo), "ROBOT36",
                            sstv_mode_name(SSTV_MODE_ROBOT36));
  gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(state->mode_combo), "MARTIN_M1",
                            sstv_mode_name(SSTV_MODE_MARTIN_M1));
  gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(state->mode_combo), "MARTIN_M2",
                            sstv_mode_name(SSTV_MODE_MARTIN_M2));
  stored_mode = g_key_file_get_string(config, "slowrx", "tx_mode", NULL);
  if (stored_mode == NULL ||
      !gtk_combo_box_set_active_id(GTK_COMBO_BOX(state->mode_combo), stored_mode))
    gtk_combo_box_set_active_id(GTK_COMBO_BOX(state->mode_combo), "ROBOT36");
  g_free(stored_mode);
  add_labeled_row(grid, "SSTV mode", state->mode_combo, row++);
  g_signal_connect(state->mode_combo, "changed", G_CALLBACK(mode_changed), state);

  stored_call = g_key_file_get_string(config, "slowrx", "tx_callsign", NULL);
  state->my_call = gtk_entry_new();
  gtk_entry_set_placeholder_text(GTK_ENTRY(state->my_call), "Your callsign");
  if (stored_call != NULL) gtk_entry_set_text(GTK_ENTRY(state->my_call), stored_call);
  g_free(stored_call);
  add_labeled_row(grid, "Your callsign", state->my_call, row++);
  g_signal_connect(state->my_call, "changed", G_CALLBACK(callsign_changed), state);

  state->their_call = gtk_entry_new();
  gtk_entry_set_placeholder_text(GTK_ENTRY(state->their_call), "Other station (decoded FSK ID, if available)");
  add_labeled_row(grid, "Station replied to", state->their_call, row++);

  state->report = gtk_entry_new();
  gtk_entry_set_placeholder_text(GTK_ENTRY(state->report), "Enter received report");
  add_labeled_row(grid, "Received report", state->report, row++);

  state->message = gtk_combo_box_text_new_with_entry();
  gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(state->message), "TNX FER QSO");
  gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(state->message), "73");
  gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(state->message), "FB SIGS");
  gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(state->message), "UR RST IS");
  gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(state->message), "CQ CQ CQ");
  message_entry = gtk_bin_get_child(GTK_BIN(state->message));
  gtk_entry_set_text(GTK_ENTRY(message_entry), "TNX FER QSO");
  add_labeled_row(grid, "Text / reply", state->message, row++);

  utc_caption = gtk_label_new("");
  state->utc_label = utc_caption;
  gtk_widget_set_halign(utc_caption, GTK_ALIGN_START);
  add_labeled_row(grid, "UTC timestamp (automatic)", utc_caption, row++);

  button = gtk_button_new_with_label("Update preview");
  gtk_grid_attach(GTK_GRID(grid), button, 1, row++, 1, 1);
  preview_box = gtk_frame_new("Composed image preview");
  state->preview = gtk_image_new();
  gtk_widget_set_size_request(state->preview, 320, 240);
  gtk_container_add(GTK_CONTAINER(preview_box), state->preview);
  gtk_grid_attach(GTK_GRID(grid), preview_box, 0, row, 2, 1);
  g_signal_connect(button, "clicked", G_CALLBACK(preview_clicked), state);

  state->status = gtk_label_new("No received image available. Choose a photo file.");
  gtk_label_set_line_wrap(GTK_LABEL(state->status), TRUE);
  gtk_widget_set_halign(state->status, GTK_ALIGN_START);
  gtk_box_pack_start(GTK_BOX(state->panel), state->status, FALSE, FALSE, 8);

  state->send_button = gtk_button_new_with_label("Transmit SSTV...");
  gtk_widget_set_halign(state->send_button, GTK_ALIGN_END);
  gtk_box_pack_end(GTK_BOX(state->panel), state->send_button, FALSE, FALSE, 8);
  g_signal_connect(state->send_button, "clicked", G_CALLBACK(send_clicked), state);

  gtk_combo_box_set_active(GTK_COMBO_BOX(state->source_combo), -1);
  gtk_widget_show_all(state->panel);
  return state->panel;
}

void sstv_tx_panel_set_received(GtkWidget *panel, GdkPixbuf *received,
                                const char *received_call, double snr_db,
                                gboolean snr_valid) {
  TxDialogState *state;
  GdkPixbuf *copy;
  gchar *report;
  int source_index;
  gboolean use_received;

  if (panel == NULL || received == NULL) return;
  state = g_object_get_data(G_OBJECT(panel), TX_STATE_KEY);
  if (state == NULL || state->destroyed) return;
  copy = gdk_pixbuf_copy(received);
  if (copy == NULL) return;

  source_index = gtk_combo_box_get_active(GTK_COMBO_BOX(state->source_combo));
  use_received = source_index != 1 || state->source == NULL;
  if (use_received && state->source != NULL) {
    g_object_unref(state->source);
    state->source = NULL;
  }
  if (state->received != NULL) g_object_unref(state->received);
  state->received = copy;
  state->has_received = TRUE;
  if (use_received) state->source = g_object_ref(state->received);

  gtk_entry_set_text(GTK_ENTRY(state->their_call),
                     received_call != NULL ? received_call : "");
  report = (snr_valid && isfinite(snr_db))
             ? g_strdup_printf("SNR %+.1f dB", snr_db) : g_strdup("");
  gtk_entry_set_text(GTK_ENTRY(state->report), report);
  g_free(report);

  if (use_received) {
    state->changing_source = TRUE;
    gtk_combo_box_set_active(GTK_COMBO_BOX(state->source_combo), 0);
    state->changing_source = FALSE;
  }
  compose_preview(state, NULL);
}
