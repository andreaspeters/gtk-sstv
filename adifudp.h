#ifndef _ADIFUDP_H_
#define _ADIFUDP_H_

#include <glib.h>

/*
 * Build an ADIF record for SSTV QSO logging.
 *
 * Parameters:
 *   call - the received callsign (required)
 *   station_call - the operator's callsign (optional, can be NULL)
 *   qso_date - date of QSO in YYYYMMDD format (8 digits) (required)
 *   time_on - time on in HHMMSS format (6 digits) (required)
 *   comment - optional comment about the QSO (can be NULL)
 *   error - pointer to GError for error reporting
 *
 * Returns:
 *   A newly allocated string with the ADIF record, or NULL on error.
 *   The caller must free the returned string with g_free().
 */
gchar *adif_build_sstv_record(const gchar *call, const gchar *station_call, const gchar *qso_date, const gchar *time_on, const gchar *comment, GError **error);

/*
 * Send an ADIF record via UDP to a remote service.
 *
 * Parameters:
 *   host - hostname or IP address (required)
 *   port - UDP port number
 *   payload - the ADIF record to send (required)
 *   error - pointer to GError for error reporting
 *
 * Returns:
 *   TRUE on success, FALSE on error.
 */
gboolean adif_udp_send(const gchar *host, guint16 port, const gchar *payload, GError **error);

#endif /* _ADIFUDP_H_ */