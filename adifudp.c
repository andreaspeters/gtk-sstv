#include <glib.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <unistd.h>

#include "adifudp.h"

/* Helper function to validate date format (YYYYMMDD) */
static gboolean validate_date(const gchar *date)
{
  if (!date || strlen(date) != 8)
    return FALSE;
  
  for (int i = 0; i < 8; i++) {
    if (!g_ascii_isdigit(date[i]))
      return FALSE;
  }
  return TRUE;
}

/* Helper function to validate time format (HHMMSS) */
static gboolean validate_time(const gchar *time)
{
  if (!time || strlen(time) != 6)
    return FALSE;
  
  for (int i = 0; i < 6; i++) {
    if (!g_ascii_isdigit(time[i]))
      return FALSE;
  }
  return TRUE;
}

/* Reject record delimiters and control characters in callsign values. */
static gboolean validate_callsign(const gchar *call)
{
  const gchar *p;
  if (call == NULL || *call == '\0')
    return FALSE;
  for (p = call; *p != '\0'; ++p) {
    if (!g_ascii_isalnum(*p) && *p != '/')
      return FALSE;
  }
  return TRUE;
}

static GQuark adif_error_quark(void)
{
  return g_quark_from_static_string("slowrx-adif-udp-error");
}

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
gchar *adif_build_sstv_record(const gchar *call, const gchar *station_call, const gchar *qso_date, const gchar *time_on, const gchar *comment, GError **error)
{
  /* Validate input parameters */
  if (!validate_callsign(call)) {
    g_set_error_literal(error, adif_error_quark(), 1,
                        "Invalid or missing callsign");
    return NULL;
  }

  if (station_call != NULL && *station_call != '\0' &&
      !validate_callsign(station_call)) {
    g_set_error_literal(error, adif_error_quark(), 1,
                        "Invalid station callsign");
    return NULL;
  }
  if (!validate_date(qso_date)) {
    g_set_error_literal(error, adif_error_quark(), 1,
                        "Invalid date format: must be YYYYMMDD");
    return NULL;
  }

  if (!validate_time(time_on)) {
    g_set_error_literal(error, adif_error_quark(), 1,
                        "Invalid time format: must be HHMMSS");
    return NULL;
  }

  /* Build the ADIF record */
  GString *record = g_string_new(NULL);
  g_string_append_printf(record, "<CALL:%" G_GSIZE_FORMAT ">%s",
                         strlen(call), call);
  if (station_call != NULL && *station_call != '\0') {
    g_string_append_printf(record, "<STATION_CALLSIGN:%" G_GSIZE_FORMAT ">%s",
                           strlen(station_call), station_call);
  }
  g_string_append_printf(record, "<QSO_DATE:%" G_GSIZE_FORMAT ">%s",
                         strlen(qso_date), qso_date);
  g_string_append_printf(record, "<TIME_ON:%" G_GSIZE_FORMAT ">%s",
                         strlen(time_on), time_on);
  g_string_append(record, "<MODE:4>SSTV");
  if (comment != NULL && *comment != '\0')
    g_string_append_printf(record, "<COMMENT:%" G_GSIZE_FORMAT ">%s",
                           strlen(comment), comment);
  g_string_append(record, "<EOR>");

  return g_string_free(record, FALSE);
}

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
gboolean adif_udp_send(const gchar *host, guint16 port, const gchar *payload, GError **error)
{
  struct addrinfo hints;
  struct addrinfo *addresses = NULL;
  struct addrinfo *address;
  gsize payload_length;
  gboolean sent = FALSE;
  int resolution_result;
  int last_error = 0;

  if (host == NULL || *host == '\0') {
    g_set_error_literal(error, adif_error_quark(), 2,
                        "Invalid or missing UDP host");
    return FALSE;
  }
  if (port == 0) {
    g_set_error_literal(error, adif_error_quark(), 2,
                        "Invalid UDP port 0");
    return FALSE;
  }
  if (payload == NULL || *payload == '\0') {
    g_set_error_literal(error, adif_error_quark(), 2,
                        "Invalid or missing UDP payload");
    return FALSE;
  }

  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_DGRAM;
  hints.ai_protocol = IPPROTO_UDP;
  resolution_result = getaddrinfo(host, NULL, &hints, &addresses);
  if (resolution_result != 0) {
    g_set_error(error, adif_error_quark(), 3,
                "Unable to resolve UDP host '%s': %s", host,
                gai_strerror(resolution_result));
    return FALSE;
  }

  payload_length = strlen(payload);
  for (address = addresses; address != NULL; address = address->ai_next) {
    struct sockaddr_storage destination;
    socklen_t destination_length;
    int sockfd;
    ssize_t bytes_sent;

    memset(&destination, 0, sizeof(destination));
    if (address->ai_family == AF_INET) {
      struct sockaddr_in *ipv4 = (struct sockaddr_in *)&destination;
      *ipv4 = *(const struct sockaddr_in *)address->ai_addr;
      ipv4->sin_port = htons(port);
      destination_length = sizeof(*ipv4);
    } else if (address->ai_family == AF_INET6) {
      struct sockaddr_in6 *ipv6 = (struct sockaddr_in6 *)&destination;
      *ipv6 = *(const struct sockaddr_in6 *)address->ai_addr;
      ipv6->sin6_port = htons(port);
      destination_length = sizeof(*ipv6);
    } else {
      continue;
    }

    sockfd = socket(address->ai_family, SOCK_DGRAM, IPPROTO_UDP);
    if (sockfd < 0) {
      last_error = errno;
      continue;
    }
    bytes_sent = sendto(sockfd, payload, payload_length, 0,
                        (const struct sockaddr *)&destination,
                        destination_length);
    if (bytes_sent == (ssize_t)payload_length) {
      sent = TRUE;
      close(sockfd);
      break;
    }
    last_error = errno != 0 ? errno : EIO;
    close(sockfd);
  }
  freeaddrinfo(addresses);

  if (!sent) {
    g_set_error(error, adif_error_quark(), 4,
                "Failed to send ADIF UDP datagram: %s",
                g_strerror(last_error != 0 ? last_error : EIO));
    return FALSE;
  }
  return TRUE;
}