#ifndef SLOWRX_ENCODER_H
#define SLOWRX_ENCODER_H

#include <stddef.h>
#include <gdk-pixbuf/gdk-pixbuf.h>

typedef struct {
  gint16 *samples;
  size_t length;
} SstvPcm;

typedef enum {
  SSTV_MODE_ROBOT36,
  SSTV_MODE_MARTIN_M1,
  SSTV_MODE_MARTIN_M2
} SstvMode;

/* Encode any RGB GdkPixbuf as Robot 36 at 44.1 kHz mono S16.
 * The returned samples belong to the caller and must be freed with free().
 * Returns FALSE and sets error on invalid input or allocation failure.
 */
gboolean sstv_encode_robot36(GdkPixbuf *image, SstvPcm *pcm, GError **error);
gboolean sstv_encode_mode(GdkPixbuf *image, SstvMode mode,
                          SstvPcm *pcm, GError **error);
const gchar *sstv_mode_name(SstvMode mode);

/* Encode FSK ID data (6-bit bytes, LSB first) as 44.1 kHz mono S16.
 * The returned samples belong to the caller and must be freed with free().
 * Returns FALSE and sets error on invalid input or allocation failure.
 * FSKID format: sync bytes (0x20, 0x2A), 6-bit data with LSB-first,
 *               add 0x20 for ASCII, terminator 0x01.
 */
gboolean sstv_encode_fskid(const gchar *callsign, SstvPcm *pcm, GError **error);
gboolean sstv_encode_cw(const gchar *callsign, SstvPcm *pcm, GError **error);
gboolean sstv_encode_video_id(const gchar *callsign, SstvPcm *pcm,
                              GError **error);
gboolean sstv_build_tx_sequence(const SstvPcm *sstv_pcm,
                                const gchar *callsign,
                                gboolean send_fskid,
                                gboolean send_video_id,
                                gboolean send_cw,
                                SstvPcm *tx_pcm,
                                GError **error);

#endif
