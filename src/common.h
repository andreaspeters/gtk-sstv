#ifndef _COMMON_H_
#define _COMMON_H_

#define MINSLANT 30
#define MAXSLANT 150
#define BUFLEN   4096
#define SYNCPIXLEN 1.5e-3

extern gboolean   Abort;
extern gboolean   Adaptive;
extern gboolean  *HasSync;
extern gboolean   ManualActivated;
extern gboolean   ManualResync;
extern gint       ApplicationStopping;
extern gboolean   listen_thread_started;
extern guchar    *StoredLum;
extern pthread_t  thread1;
extern guchar     VISmap[];

typedef struct _FFTStuff FFTStuff;
struct _FFTStuff {
  double       *in;
  fftw_complex *out;
  fftw_plan     Plan1024;
  fftw_plan     Plan2048;
};
extern FFTStuff fft;

typedef struct _PcmData PcmData;
struct _PcmData {
  snd_pcm_t *handle;
  gint16    *Buffer;
  int        Channels;
  int        WindowPtr;
  gboolean   BufferDrop;
};
extern PcmData pcm;

typedef struct _GuiObjs GuiObjs;
struct _GuiObjs {
  GtkWidget *button_abort;
  GtkWidget *button_adif;
  GtkWidget *button_answer;
  GtkWidget *button_browse;
  GtkWidget *button_clear;

  GtkWidget *button_start;
  GtkWidget *combo_card;
  GtkWidget *combo_mode;
  GtkWidget *entry_picdir;
  GtkWidget *eventbox_img;
  GtkWidget *frame_manual;
  GtkWidget *frame_slant;
  GtkWidget *grid_vu;
  GtkWidget *saved_view;
  GtkWidget *image_devstatus;
  GtkWidget *image_pwr;
  GtkWidget *image_rx;
  GtkWidget *image_snr;
  GtkWidget *image_waterfall;
  GtkWidget *label_fskid;
  GtkWidget *label_lastmode;
  GtkWidget *label_waterfall;
  GtkWidget *label_utc;
  GtkWidget *menuitem_about;
  GtkWidget *menuitem_quit;
  GtkWidget *spin_shift;
  GtkWidget *statusbar;
  GtkWidget *tog_adapt;
  GtkWidget *tog_fsk;
  GtkWidget *tog_rx;
  GtkWidget *tog_save;
  GtkWidget *tog_setedge;
  GtkWidget *tog_slant;
  GtkWidget *window_about;
  GtkWidget *window_main;
  GtkWidget *tx_panel;
};
extern GuiObjs   gui;

extern GdkPixbuf *pixbuf_PWR;
extern GdkPixbuf *pixbuf_SNR;
extern GdkPixbuf *pixbuf_rx;
extern GdkPixbuf *pixbuf_disp;

void gui_image_set_pixbuf(GtkImage *image, GdkPixbuf *pixbuf);
void gui_picture_set_pixbuf(GtkPicture *picture, GdkPixbuf *pixbuf);
GdkTexture *gui_texture_new_from_pixbuf(GdkPixbuf *pixbuf);

extern GListStore *savedstore;

extern GKeyFile  *config;

typedef enum {
  GUI_ACTION_SENSITIVE,
  GUI_ACTION_LABEL_TEXT,
  GUI_ACTION_LABEL_MARKUP,
  GUI_ACTION_STATUS,
  GUI_ACTION_TOGGLE_ACTIVE,
  GUI_ACTION_COMBO_ACTIVE,
  GUI_ACTION_SPIN_VALUE,
  GUI_ACTION_TOOLTIP,
  GUI_ACTION_DEVICE_STATUS
} GuiAction;

typedef enum {
  GUI_OPTION_RECEIVE = 1,
  GUI_OPTION_FSK = 2,
  GUI_OPTION_SLANT = 4,
  GUI_OPTION_SAVE = 8,
  GUI_OPTION_ADAPTIVE = 16,
  GUI_OPTION_MODE_INDEX = 100,
  GUI_OPTION_SHIFT = 101
} GuiOption;

void gui_dispatch(GuiAction action, GtkWidget *widget, const gchar *text,
                  const gchar *detail, gint int_value, gdouble double_value,
                  gboolean boolean_value);
void gui_option_set_toggle(GuiOption option, gboolean active);
gboolean gui_option_get_toggle(GuiOption option);
void gui_option_set_integer(GuiOption option, gint value);
gint gui_option_get_integer(GuiOption option);


typedef struct _PicMeta PicMeta;
struct _PicMeta {
  gshort HedrShift;
  guchar Mode;
  double Rate;
  double SnrDb;
  gboolean SnrValid;
  gboolean ReceivedValid;
  int    Skip;
  GdkPixbuf *thumbbuf;
  char   timestr[40];
  char   RemoteCall[20];
};
extern PicMeta CurrentPic;

// SSTV modes
enum {
  UNKNOWN=0,
  M1,    M2,   M3,    M4,
  S1,    S2,   SDX,
  R72,   R36,  R24,   R24BW, R12BW, R8BW,
  PD50,  PD90, PD120, PD160, PD180, PD240, PD290,
  P3,    P5,   P7,
  W2120, W2180
};

// Color encodings
enum {
  GBR, RGB, YUV, BW
};

typedef struct ModeSpec {
  char   *Name;
  char   *ShortName;
  double  SyncTime;
  double  PorchTime;
  double  SeptrTime;
  double  PixelTime;
  double  LineTime;
  gushort ImgWidth;
  gushort NumLines;
  guchar  LineHeight;
  guchar  ColorEnc;
} _ModeSpec;

extern _ModeSpec ModeSpec[];

double   power     (fftw_complex coeff);
guchar   clip          (double a);
void     createGUI     (GtkApplication *application);
double   deg2rad       (double Deg);
double   FindSync      (guchar Mode, double Rate, int *Skip, gsize SyncCount);
gboolean sync_sample_value(const gboolean *samples, gsize count, double index);
void     GetFSK        (char *dest);
gboolean GetVideo      (guchar Mode, double Rate, int Skip, gboolean Redraw);
guchar   GetVIS        ();
guint    GetBin        (double Freq, guint FFTLen);
int      initPcmDevice (char *wanteddevname);
void     *Listen       ();
void     populateDeviceList ();
void     readPcm       (gint numsamples);
void     saveCurrentPic();
void     setVU         (double *Power, int FFTLen, int WinIdx, gboolean ShowWin);
void     gui_waterfall_init(void);
void     gui_waterfall_update_samples(guint samples_read);
void     gui_waterfall_shutdown(void);
void     gui_set_image_pixbuf(GtkWidget *image, GdkPixbuf *pixbuf);

void     evt_AbortRx       ();
void     evt_changeDevices (GObject *object, GParamSpec *pspec,
                            gpointer user_data);
void     evt_chooseDir     (GtkButton *button, gpointer user_data);
void     evt_clearPix      ();
void     evt_clickimg      (GtkGestureClick *gesture, gint n_press,
                            gdouble x, gdouble y, gpointer user_data);
void     evt_deletewindow  (GtkWidget *widget, gpointer user_data);
void     evt_GetAdaptive   ();
void     evt_ManualStart   ();
void     evt_show_about    (GtkWidget *item, gpointer user_data);
#endif
