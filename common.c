#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <gtk/gtk.h>
#include <alsa/asoundlib.h>

#include <fftw3.h>

#include "common.h"

gboolean     Abort           = FALSE;
gboolean     Adaptive        = TRUE;
gboolean    *HasSync         = NULL;
gshort       HedrShift       = 0;
gboolean     ManualActivated = FALSE;
gboolean     ManualResync    = FALSE;
guchar      *StoredLum       = NULL;
gint         ApplicationStopping = FALSE;
gboolean     listen_thread_started = FALSE;

pthread_t    thread1;

FFTStuff     fft;
GuiObjs      gui;
PicMeta      CurrentPic;
PcmData      pcm;

GdkPixbuf   *pixbuf_rx       = NULL;
GdkPixbuf   *pixbuf_disp     = NULL;
GdkPixbuf   *pixbuf_PWR      = NULL;
GdkPixbuf   *pixbuf_SNR      = NULL;

GListStore *savedstore       = NULL;

GKeyFile    *config          = NULL;
static volatile gint gui_option_bits = 0;
static volatile gint gui_option_mode = 0;
static volatile gint gui_option_shift = 0;

GdkTexture *gui_texture_new_from_pixbuf(GdkPixbuf *pixbuf) {
  GdkMemoryFormat format;
  GBytes *bytes;
  GdkTexture *texture;
  gint width, height, rowstride, channels;
  gboolean has_alpha;

  if (pixbuf == NULL || !GDK_IS_PIXBUF(pixbuf) ||
      gdk_pixbuf_get_colorspace(pixbuf) != GDK_COLORSPACE_RGB ||
      gdk_pixbuf_get_bits_per_sample(pixbuf) != 8)
    return NULL;

  width = gdk_pixbuf_get_width(pixbuf);
  height = gdk_pixbuf_get_height(pixbuf);
  rowstride = gdk_pixbuf_get_rowstride(pixbuf);
  channels = gdk_pixbuf_get_n_channels(pixbuf);
  has_alpha = gdk_pixbuf_get_has_alpha(pixbuf);
  if (width <= 0 || height <= 0 || rowstride <= 0 ||
      channels != (has_alpha ? 4 : 3))
    return NULL;

  format = has_alpha ? GDK_MEMORY_R8G8B8A8 : GDK_MEMORY_R8G8B8;
  bytes = g_bytes_new(gdk_pixbuf_get_pixels(pixbuf),
                      (gsize)rowstride * (gsize)height);
  texture = gdk_memory_texture_new(width, height, format, bytes,
                                   (gsize)rowstride);
  g_bytes_unref(bytes);
  return texture;
}

void gui_image_set_pixbuf(GtkImage *image, GdkPixbuf *pixbuf) {
  GdkTexture *texture = gui_texture_new_from_pixbuf(pixbuf);
  gtk_image_set_from_paintable(image,
                               texture != NULL ? GDK_PAINTABLE(texture) : NULL);
  g_clear_object(&texture);
}

void gui_picture_set_pixbuf(GtkPicture *picture, GdkPixbuf *pixbuf) {
  GdkTexture *texture = gui_texture_new_from_pixbuf(pixbuf);
  gtk_picture_set_paintable(picture,
                            texture != NULL ? GDK_PAINTABLE(texture) : NULL);
  g_clear_object(&texture);
}

static gint gui_option_mask(GuiOption option) {
  switch (option) {
    case GUI_OPTION_RECEIVE: return 1;
    case GUI_OPTION_FSK: return 2;
    case GUI_OPTION_SLANT: return 4;
    case GUI_OPTION_SAVE: return 8;
    case GUI_OPTION_ADAPTIVE: return 16;
    default: return 0;
  }
}

void gui_option_set_toggle(GuiOption option, gboolean active) {
  gint mask = gui_option_mask(option);
  gint previous, updated;
  if (mask == 0) return;
  for (;;) {
    previous = g_atomic_int_get(&gui_option_bits);
    updated = active ? previous | mask : previous & ~mask;
    if (g_atomic_int_compare_and_exchange(&gui_option_bits, previous, updated))
      return;
  }
}

gboolean gui_option_get_toggle(GuiOption option) {
  gint mask = gui_option_mask(option);
  return mask != 0 && (g_atomic_int_get(&gui_option_bits) & mask) != 0;
}

void gui_option_set_integer(GuiOption option, gint value) {
  if (option == GUI_OPTION_MODE_INDEX)
    g_atomic_int_set(&gui_option_mode, value);
  else if (option == GUI_OPTION_SHIFT)
    g_atomic_int_set(&gui_option_shift, value);
}

gint gui_option_get_integer(GuiOption option) {
  if (option == GUI_OPTION_MODE_INDEX)
    return g_atomic_int_get(&gui_option_mode);
  if (option == GUI_OPTION_SHIFT)
    return g_atomic_int_get(&gui_option_shift);
  return 0;
}

typedef struct {
  GuiAction action;
  GtkWidget *widget;
  gchar *text;
  gchar *detail;
  gint int_value;
  gdouble double_value;
  gboolean boolean_value;
} GuiDispatch;

static gboolean apply_gui_dispatch(gpointer data) {
  GuiDispatch *request = data;

  switch (request->action) {
    case GUI_ACTION_SENSITIVE:
      gtk_widget_set_sensitive(request->widget, request->boolean_value);
      break;
    case GUI_ACTION_LABEL_TEXT:
      gtk_label_set_text(GTK_LABEL(request->widget), request->text);
      break;
    case GUI_ACTION_LABEL_MARKUP:
      gtk_label_set_markup(GTK_LABEL(request->widget), request->text);
      break;
    case GUI_ACTION_STATUS:
      gtk_label_set_text(GTK_LABEL(request->widget), request->text);
      break;
    case GUI_ACTION_TOGGLE_ACTIVE:
      gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(request->widget),
                                   request->boolean_value);
      break;
    case GUI_ACTION_COMBO_ACTIVE:
      gtk_drop_down_set_selected(GTK_DROP_DOWN(request->widget),
                                 (guint)request->int_value);
      break;
    case GUI_ACTION_SPIN_VALUE:
      gtk_spin_button_set_value(GTK_SPIN_BUTTON(request->widget),
                                request->double_value);
      break;
    case GUI_ACTION_TOOLTIP:
      gtk_widget_set_tooltip_text(request->widget, request->text);
      break;
    case GUI_ACTION_DEVICE_STATUS:
      gtk_image_set_from_icon_name(GTK_IMAGE(request->widget), request->text);
      gtk_image_set_pixel_size(GTK_IMAGE(request->widget), 16);
      gtk_widget_set_tooltip_text(request->widget, request->detail);
      break;
  }
  if (request->widget != NULL) g_object_unref(request->widget);
  g_free(request->text);
  g_free(request->detail);
  g_free(request);
  return G_SOURCE_REMOVE;
}

void gui_dispatch(GuiAction action, GtkWidget *widget, const gchar *text,
                  const gchar *detail, gint int_value, gdouble double_value,
                  gboolean boolean_value) {
  GuiDispatch *request;
  if (g_atomic_int_get(&ApplicationStopping) || widget == NULL)
    return;

  request = g_new0(GuiDispatch, 1);
  request->action = action;
  request->widget = g_object_ref(widget);
  request->text = g_strdup(text);
  request->detail = g_strdup(detail);
  request->int_value = int_value;
  request->double_value = double_value;
  request->boolean_value = boolean_value;
  g_main_context_invoke(NULL, apply_gui_dispatch, request);
}

// Return the FFT bin index matching the given frequency
guint GetBin (double Freq, guint FFTLen) {
  return (Freq / 44100 * FFTLen);
}

// Sinusoid power from complex DFT coefficients
double power (fftw_complex coeff) {
  return pow(coeff[0],2) + pow(coeff[1],2);
}

// Clip to [0..255]
guchar clip (double a) {
  if      (a < 0)   return 0;
  else if (a > 255) return 255;
  return  (guchar)round(a);
}

// Convert degrees -> radians
double deg2rad (double Deg) {
  return (Deg / 180) * M_PI;
}

// Convert radians -> degrees
double rad2deg (double rad) {
  return (180 / M_PI) * rad;
}

void ensure_dir_exists(const char *dir) {
  struct stat buf;

  int i = stat(dir, &buf);
  if (i != 0) {
    if (mkdir(dir, 0777) != 0) {
      perror("Unable to create directory for output file");
      exit(EXIT_FAILURE);
    }
  }
}

// Save current picture as PNG
void saveCurrentPic() {
  GdkPixbuf *scaledpb;
  GString   *pngfilename;

  pngfilename = g_string_new(g_key_file_get_string(config,"slowrx","rxdir",NULL));
  g_string_append_printf(pngfilename, "/%s_%s.png", CurrentPic.timestr, ModeSpec[CurrentPic.Mode].ShortName);
  printf("  Saving to %s\n", pngfilename->str);

  scaledpb = gdk_pixbuf_scale_simple (pixbuf_rx, ModeSpec[CurrentPic.Mode].ImgWidth,
    ModeSpec[CurrentPic.Mode].NumLines * ModeSpec[CurrentPic.Mode].LineHeight, GDK_INTERP_HYPER);

  ensure_dir_exists(g_key_file_get_string(config,"slowrx","rxdir",NULL));
  gdk_pixbuf_savev(scaledpb, pngfilename->str, "png", NULL, NULL, NULL);
  g_object_unref(scaledpb);
  g_string_free(pngfilename, TRUE);
}


/*** Gtk+ event handlers ***/


// Quit the GTK application.
void evt_deletewindow(GtkWidget *widget, gpointer user_data) {
  GApplication *application = g_application_get_default();
  (void)widget;
  (void)user_data;
  g_atomic_int_set(&ApplicationStopping, TRUE);
  Abort = TRUE;
  if (application != NULL)
    g_application_quit(application);
}

// Transform the NoiseAdapt toggle state into a variable
void evt_GetAdaptive() {
  gboolean active = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(gui.tog_adapt));
  gui_option_set_toggle(GUI_OPTION_ADAPTIVE, active);
  g_atomic_int_set((gint *)&Adaptive, active);
}

// Manual Start clicked
void evt_ManualStart() {
  ManualActivated = TRUE;
}

// Abort clicked during rx
void evt_AbortRx() {
  Abort = TRUE;
}

// Another device selected from list
void evt_changeDevices(GObject *object, GParamSpec *pspec, gpointer user_data) {

  int status;
  GtkStringObject *selected_item;
  gchar *device_name;

  (void)object;
  (void)pspec;
  (void)user_data;

  selected_item = GTK_STRING_OBJECT(
      gtk_drop_down_get_selected_item(GTK_DROP_DOWN(gui.combo_card)));
  device_name = g_strdup(selected_item != NULL
                             ? gtk_string_object_get_string(selected_item)
                             : "default");

  pcm.BufferDrop = FALSE;
  Abort = TRUE;

  if (listen_thread_started) {
    pthread_join(thread1, NULL);
    listen_thread_started = FALSE;
  }

  if (pcm.handle != NULL) snd_pcm_close(pcm.handle);

  status = initPcmDevice(device_name);


  switch(status) {
    case 0:
      gtk_image_set_from_icon_name(GTK_IMAGE(gui.image_devstatus), "emblem-ok-symbolic");
      gtk_image_set_pixel_size(GTK_IMAGE(gui.image_devstatus), 16);
      gtk_widget_set_tooltip_text(gui.image_devstatus, "Device successfully opened");
      break;
    case -1:
      gtk_image_set_from_icon_name(GTK_IMAGE(gui.image_devstatus), "dialog-warning-symbolic");
      gtk_image_set_pixel_size(GTK_IMAGE(gui.image_devstatus), 16);
      gtk_widget_set_tooltip_text(gui.image_devstatus, "Device was opened, but doesn't support 44100 Hz");
      break;
    case -2:
      gtk_image_set_from_icon_name(GTK_IMAGE(gui.image_devstatus), "dialog-error-symbolic");
      gtk_image_set_pixel_size(GTK_IMAGE(gui.image_devstatus), 16);
      gtk_widget_set_tooltip_text(gui.image_devstatus, "Failed to open device");
      break;
  }

  g_key_file_set_string(config, "slowrx", "device", device_name);
  g_free(device_name);

  if (pthread_create(&thread1, NULL, Listen, NULL) == 0)
    listen_thread_started = TRUE;

}

// Clear received picture & metadata
void evt_clearPix() {
  CurrentPic.ReceivedValid = FALSE;
  CurrentPic.RemoteCall[0] = '\0';
  if (gui.button_answer != NULL)
    gtk_widget_set_sensitive(gui.button_answer, FALSE);
  if (gui.button_adif != NULL)
    gtk_widget_set_sensitive(gui.button_adif, FALSE);
  gdk_pixbuf_fill (pixbuf_disp, 0);
  gui_picture_set_pixbuf(GTK_PICTURE(gui.image_rx), pixbuf_disp);
  gtk_label_set_markup (GTK_LABEL(gui.label_fskid), "");
  gtk_label_set_markup (GTK_LABEL(gui.label_utc), "");
  gtk_label_set_markup (GTK_LABEL(gui.label_lastmode), "");
}

// Manual slant adjust
void evt_clickimg(GtkGestureClick *gesture, gint n_press, gdouble event_x,
                  gdouble event_y, gpointer user_data) {
  static double prevx=0,prevy=0,newrate;
  static gboolean   secondpress=FALSE;
  double        x,y,dx,dy,xic;

  (void)gesture;
  (void)n_press;
  (void)user_data;

  if (gtk_toggle_button_get_active (GTK_TOGGLE_BUTTON(gui.tog_setedge))) {

    x = event_x * (ModeSpec[CurrentPic.Mode].ImgWidth / 500.0);
    y = event_y * (ModeSpec[CurrentPic.Mode].ImgWidth / 500.0) / ModeSpec[CurrentPic.Mode].LineHeight;

    if (secondpress) {
      secondpress=FALSE;

      dx = x - prevx;
      dy = y - prevy;

      gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON(gui.tog_setedge),FALSE);

      // Adjust sample rate, if in sensible limits
      newrate = CurrentPic.Rate + CurrentPic.Rate * (dx * ModeSpec[CurrentPic.Mode].PixelTime) / (dy * ModeSpec[CurrentPic.Mode].LineHeight * ModeSpec[CurrentPic.Mode].LineTime);
      if (newrate > 32000 && newrate < 56000) {
        CurrentPic.Rate = newrate;

        // Find x-intercept and adjust skip
        xic = fmod( (x - (y / (dy/dx))), ModeSpec[CurrentPic.Mode].ImgWidth);
        if (xic < 0) xic = ModeSpec[CurrentPic.Mode].ImgWidth - xic;
        CurrentPic.Skip = fmod(CurrentPic.Skip + xic * ModeSpec[CurrentPic.Mode].PixelTime * CurrentPic.Rate,
          ModeSpec[CurrentPic.Mode].LineTime * CurrentPic.Rate);
        if (CurrentPic.Skip > ModeSpec[CurrentPic.Mode].LineTime * CurrentPic.Rate / 2.0)
          CurrentPic.Skip -= ModeSpec[CurrentPic.Mode].LineTime * CurrentPic.Rate;

        // Signal the listener to exit from GetVIS() and re-process the pic
        ManualResync = TRUE;
      }

    } else {
      secondpress = TRUE;
      prevx = x;
      prevy = y;
    }
  } else {
    secondpress=FALSE;
    gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON(gui.tog_setedge), FALSE);
  }
}
