/*
 * slowrx - an SSTV decoder
 * * * * * * * * * * * * * *
 * 
 */

#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <gtk/gtk.h>
#include <pthread.h>

#include <alsa/asoundlib.h>

#include <fftw3.h>

#include "common.h"
#include "txdialog.h"

typedef struct {
  GdkPixbuf *thumbnail;
  GdkPixbuf *received;
  GtkWidget *tx_panel;
  gchar *id;
  gchar *callsign;
  double snr_db;
  gboolean received_valid;
  gboolean snr_valid;
} ReceiveUiUpdate;

#define RECENT_PICTURE_THUMBNAIL_WIDTH 245

static GdkPixbuf *create_recent_picture_thumbnail(GdkPixbuf *image,
                                                   guint mode) {
  gint height = (gint)(RECENT_PICTURE_THUMBNAIL_WIDTH /
                       (double)ModeSpec[mode].ImgWidth *
                       ModeSpec[mode].NumLines * ModeSpec[mode].LineHeight);
  return gdk_pixbuf_scale_simple(image, RECENT_PICTURE_THUMBNAIL_WIDTH,
                                 height, GDK_INTERP_HYPER);
}

static gboolean apply_receive_ui_update(gpointer data) {
  ReceiveUiUpdate *update = data;

  if (savedstore != NULL && update->thumbnail != NULL) {
    GtkStringObject *item = gtk_string_object_new(
        update->id != NULL ? update->id : "");
    GdkTexture *texture = gui_texture_new_from_pixbuf(update->thumbnail);
    if (texture != NULL)
      g_object_set_data_full(G_OBJECT(item), "slowrx-thumbnail", texture,
                             g_object_unref);
    g_list_store_insert(savedstore, 0, item);
    g_object_unref(item);
  }
  if (update->tx_panel != NULL) {
    sstv_tx_panel_set_received(update->tx_panel, update->received,
                               update->callsign, update->snr_db,
                               update->snr_valid);
  }
  if (gui.button_answer != NULL)
    gtk_widget_set_sensitive(gui.button_answer, update->received_valid);
  if (gui.button_adif != NULL)
    gtk_widget_set_sensitive(gui.button_adif,
                             update->received_valid && update->callsign[0] != '\0');

  g_clear_object(&update->thumbnail);
  g_clear_object(&update->received);
  g_clear_object(&update->tx_panel);
  g_free(update->id);
  g_free(update->callsign);
  g_free(update);
  return G_SOURCE_REMOVE;
}

static void queue_receive_ui_update(const gchar *id) {
  ReceiveUiUpdate *update = g_new0(ReceiveUiUpdate, 1);
  update->thumbnail = CurrentPic.thumbbuf != NULL
      ? g_object_ref(CurrentPic.thumbbuf) : NULL;
  update->received = CurrentPic.ReceivedValid && pixbuf_rx != NULL
      ? g_object_ref(pixbuf_rx) : NULL;
  update->tx_panel = gui.tx_panel != NULL ? g_object_ref(gui.tx_panel) : NULL;
  update->id = g_strdup(id != NULL ? id : "");
  update->callsign = g_strdup(CurrentPic.RemoteCall);
  update->snr_db = CurrentPic.SnrDb;
  update->received_valid = CurrentPic.ReceivedValid;
  update->snr_valid = CurrentPic.SnrValid;
  g_main_context_invoke(NULL, apply_receive_ui_update, update);
}

// The thread that listens to VIS headers and calls decoders etc
void *Listen() {

  char        rctime[8];

  guchar      Mode=0;
  struct tm  *timeptr = NULL;
  time_t      timet;
  gboolean    Finished;
  char        id[20];
  GtkTreeIter iter;
  gsize       SyncCount;

  while (TRUE) {

    if (g_atomic_int_get(&ApplicationStopping))
      return NULL;

    gui_dispatch(GUI_ACTION_SENSITIVE, gui.grid_vu, NULL, NULL, 0, 0, TRUE);
    gui_dispatch(GUI_ACTION_SENSITIVE, gui.button_abort, NULL, NULL, 0, 0, FALSE);
    gui_dispatch(GUI_ACTION_SENSITIVE, gui.button_clear, NULL, NULL, 0, 0, TRUE);

    pcm.WindowPtr = 0;
    snd_pcm_prepare(pcm.handle);
    snd_pcm_start  (pcm.handle);
    Abort = FALSE;

    do {

      // Wait for VIS
      Mode = GetVIS();

      if (g_atomic_int_get(&ApplicationStopping))
        return NULL;

      // Stop listening on ALSA error
      if (Abort) pthread_exit(NULL);

      // If manual resync was requested, redraw image
      if (ManualResync) {
        ManualResync = FALSE;
        snd_pcm_drop(pcm.handle);
        printf("getvideo at %.2f skip %d\n",CurrentPic.Rate,CurrentPic.Skip);
        GetVideo(CurrentPic.Mode, CurrentPic.Rate, CurrentPic.Skip, TRUE);
        if (gui_option_get_toggle(GUI_OPTION_SAVE))
          saveCurrentPic();
        pcm.WindowPtr = 0;
        snd_pcm_prepare(pcm.handle);
        snd_pcm_start  (pcm.handle);
      }

    } while (Mode == 0);

    // Start reception
    
    CurrentPic.Rate = 44100;
    CurrentPic.Mode = Mode;
    CurrentPic.ReceivedValid = FALSE;
    CurrentPic.RemoteCall[0] = '\0';

    printf("  ==== %s ====\n", ModeSpec[CurrentPic.Mode].Name);

    // Store time of reception
    timet = time(NULL);
    timeptr = gmtime(&timet);
    strftime(CurrentPic.timestr, sizeof(CurrentPic.timestr)-1,"%Y%m%d-%H%M%Sz", timeptr);
    

    // Allocate space for cached Lum
    free(StoredLum);
    StoredLum = calloc( (int)((ModeSpec[CurrentPic.Mode].LineTime * ModeSpec[CurrentPic.Mode].NumLines + 1) * 44100), sizeof(guchar));
    if (StoredLum == NULL) {
      perror("Listen: Unable to allocate memory for Lum");
      exit(EXIT_FAILURE);
    }

    // Allocate space for sync signal
    SyncCount = (gsize)(ModeSpec[CurrentPic.Mode].LineTime * ModeSpec[CurrentPic.Mode].NumLines / (13.0/44100) + 1);
    HasSync = calloc(SyncCount, sizeof(gboolean));
    if (HasSync == NULL) {
      perror("Listen: Unable to allocate memory for sync signal");
      exit(EXIT_FAILURE);
    }
  
    // Get video
    strftime(rctime,  sizeof(rctime)-1, "%H:%Mz", timeptr);
    gui_dispatch(GUI_ACTION_LABEL_TEXT, gui.label_fskid, "", NULL, 0, 0, FALSE);
    gui_dispatch(GUI_ACTION_SENSITIVE, gui.frame_manual, NULL, NULL, 0, 0, FALSE);
    gui_dispatch(GUI_ACTION_SENSITIVE, gui.frame_slant, NULL, NULL, 0, 0, FALSE);
    gui_dispatch(GUI_ACTION_SENSITIVE, gui.combo_card, NULL, NULL, 0, 0, FALSE);
    gui_dispatch(GUI_ACTION_SENSITIVE, gui.button_abort, NULL, NULL, 0, 0, TRUE);
    gui_dispatch(GUI_ACTION_SENSITIVE, gui.button_clear, NULL, NULL, 0, 0, FALSE);
    if (gui.button_answer != NULL)
      gui_dispatch(GUI_ACTION_SENSITIVE, gui.button_answer, NULL, NULL, 0, 0, FALSE);
    if (gui.button_adif != NULL)
      gui_dispatch(GUI_ACTION_SENSITIVE, gui.button_adif, NULL, NULL, 0, 0, FALSE);
    gui_dispatch(GUI_ACTION_TOGGLE_ACTIVE, gui.tog_setedge, NULL, NULL, 0, 0, FALSE);
    gui_dispatch(GUI_ACTION_STATUS, gui.statusbar, "Receiving video...", NULL, 0, 0, FALSE);
    gui_dispatch(GUI_ACTION_LABEL_MARKUP, gui.label_lastmode,
                 ModeSpec[CurrentPic.Mode].Name, NULL, 0, 0, FALSE);
    gui_dispatch(GUI_ACTION_LABEL_MARKUP, gui.label_utc, rctime, NULL, 0, 0, FALSE);
    printf("  getvideo @ %.1f Hz, Skip %d, HedrShift %+d Hz\n", 44100.0, 0, CurrentPic.HedrShift);

    Finished = GetVideo(CurrentPic.Mode, 44100, 0, FALSE);
    CurrentPic.ReceivedValid = Finished;

    gui_dispatch(GUI_ACTION_SENSITIVE, gui.button_abort, NULL, NULL, 0, 0, FALSE);
    
    id[0] = '\0';

    if (Finished && gui_option_get_toggle(GUI_OPTION_FSK)) {
      gui_dispatch(GUI_ACTION_STATUS, gui.statusbar, "Receiving FSK ID...",
                   NULL, 0, 0, FALSE);
      GetFSK(id);
      g_strlcpy(CurrentPic.RemoteCall, id, sizeof(CurrentPic.RemoteCall));
      printf("  FSKID \"%s\"\n",id);
      gui_dispatch(GUI_ACTION_LABEL_TEXT, gui.label_fskid, id, NULL, 0, 0, FALSE);
    }

    snd_pcm_drop(pcm.handle);

    if (Finished && gui_option_get_toggle(GUI_OPTION_SLANT)) {

      // Fix slant
      //setVU(0,6);
      gui_dispatch(GUI_ACTION_STATUS, gui.statusbar, "Calculating slant...",
                   NULL, 0, 0, FALSE);
      gui_dispatch(GUI_ACTION_SENSITIVE, gui.grid_vu, NULL, NULL, 0, 0, FALSE);
      printf("  FindSync @ %.1f Hz\n",CurrentPic.Rate);
      CurrentPic.Rate = FindSync(CurrentPic.Mode, CurrentPic.Rate,
                                 &CurrentPic.Skip, SyncCount);
   
      // Final image  
      printf("  getvideo @ %.1f Hz, Skip %d, HedrShift %+d Hz\n", CurrentPic.Rate, CurrentPic.Skip, CurrentPic.HedrShift);
      GetVideo(CurrentPic.Mode, CurrentPic.Rate, CurrentPic.Skip, TRUE);
    }

    free (HasSync);
    HasSync = NULL;

    // Add thumbnail to iconview
    CurrentPic.thumbbuf = create_recent_picture_thumbnail(
        pixbuf_rx, CurrentPic.Mode);
    queue_receive_ui_update(id);

    // Save PNG
    if (gui_option_get_toggle(GUI_OPTION_SAVE)) {
    
      //setVU(0,6);

      /*ensure_dir_exists("rx-lum");
      LumFile = fopen(lumfilename,"w");
      if (LumFile == NULL)
        perror("Unable to open luma file for writing");
      fwrite(StoredLum,1,(ModeSpec[Mode].LineTime * ModeSpec[Mode].NumLines) * 44100,LumFile);
      fclose(LumFile);*/

      saveCurrentPic();
    }
       
    gui_dispatch(GUI_ACTION_SENSITIVE, gui.frame_slant, NULL, NULL, 0, 0, TRUE);
    gui_dispatch(GUI_ACTION_SENSITIVE, gui.frame_manual, NULL, NULL, 0, 0, TRUE);
    gui_dispatch(GUI_ACTION_SENSITIVE, gui.combo_card, NULL, NULL, 0, 0, TRUE);
    
  }
}


static GString *config_path;
static gboolean application_initialized;

static void application_activate(GtkApplication *application, gpointer user_data) {
  const gchar *config_dir;
  (void)user_data;

  if (application_initialized) {
    if (gui.window_main != NULL)
      gtk_window_present(GTK_WINDOW(gui.window_main));
    return;
  }
  application_initialized = TRUE;

  config_dir = g_get_user_config_dir();
  config_path = g_string_new(config_dir);
  g_string_append(config_path, "/slowrx.ini");

  config = g_key_file_new();
  if (!g_key_file_load_from_file(config, config_path->str,
                                 G_KEY_FILE_KEEP_COMMENTS, NULL)) {
    printf("No valid config file found\n");
    g_key_file_load_from_data(config, "[slowrx]\ndevice=default", -1,
                              G_KEY_FILE_NONE, NULL);
  }

  fft.in = fftw_alloc_real(2048);
  if (fft.in == NULL) {
    perror("GetVideo: Unable to allocate memory for FFT");
    g_application_quit(G_APPLICATION(application));
    return;
  }
  fft.out = fftw_alloc_complex(2048);
  if (fft.out == NULL) {
    perror("GetVideo: Unable to allocate memory for FFT");
    fftw_free(fft.in);
    fft.in = NULL;
    g_application_quit(G_APPLICATION(application));
    return;
  }
  memset(fft.in, 0, sizeof(double) * 2048);
  fft.Plan1024 = fftw_plan_dft_r2c_1d(1024, fft.in, fft.out, FFTW_ESTIMATE);
  fft.Plan2048 = fftw_plan_dft_r2c_1d(2048, fft.in, fft.out, FFTW_ESTIMATE);

  createGUI(application);
  populateDeviceList();
  sstv_tx_panel_refresh_audio_devices(gui.tx_panel);
}

int main(int argc, char *argv[]) {
  GtkApplication *application;
  FILE *config_file;
  gchar *config_data;
  gsize config_length;
  int status;

  application = gtk_application_new("org.slowrx.SlowRx",
                                    G_APPLICATION_DEFAULT_FLAGS);
  g_signal_connect(application, "activate",
                   G_CALLBACK(application_activate), NULL);
  status = g_application_run(G_APPLICATION(application), argc, argv);

  g_atomic_int_set(&ApplicationStopping, TRUE);
  Abort = TRUE;
  if (listen_thread_started) {
    pthread_join(thread1, NULL);
    listen_thread_started = FALSE;
  }

  if (config != NULL && config_path != NULL) {
    config_file = fopen(config_path->str, "w");
    if (config_file == NULL) {
      perror("Unable to open config file for writing");
    } else {
      config_data = g_key_file_to_data(config, &config_length, NULL);
      if (config_data != NULL) {
        if (fwrite(config_data, 1, config_length, config_file) != config_length)
          perror("Unable to write config file");
        g_free(config_data);
      }
      fclose(config_file);
    }
    g_key_file_unref(config);
  }
  if (config_path != NULL)
    g_string_free(config_path, TRUE);

  g_clear_object(&pixbuf_rx);
  g_clear_object(&pixbuf_disp);
  g_clear_object(&pixbuf_PWR);
  g_clear_object(&pixbuf_SNR);
  gui_waterfall_shutdown();
  free(StoredLum);
  if (fft.Plan1024 != NULL) fftw_destroy_plan(fft.Plan1024);
  if (fft.Plan2048 != NULL) fftw_destroy_plan(fft.Plan2048);
  if (fft.in != NULL) fftw_free(fft.in);
  if (fft.out != NULL) fftw_free(fft.out);
  g_object_unref(application);
  return status;
}
