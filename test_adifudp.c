#include <glib.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#include <string.h>

#include "adifudp.h"

static void test_build_complete_sstv_record(void) {
  GError *error = NULL;
  gchar *record = adif_build_sstv_record("K1ABC", "W1XYZ", "20230415",
                                          "143000", "Test comment", &error);
  g_assert_no_error(error);
  g_assert_nonnull(record);
  g_assert_cmpstr(record, ==,
      "<CALL:5>K1ABC<STATION_CALLSIGN:5>W1XYZ<QSO_DATE:8>20230415"
      "<TIME_ON:6>143000<MODE:4>SSTV<COMMENT:12>Test comment<EOR>");
  g_assert_true(g_str_has_prefix(record, "<CALL"));
  g_assert_true(g_str_has_suffix(record, "<EOR>"));
  g_free(record);
}

static void test_build_record_without_optional_fields(void) {
  GError *error = NULL;
  gchar *record = adif_build_sstv_record("K1ABC", NULL, "20230415",
                                          "143000", NULL, &error);
  g_assert_no_error(error);
  g_assert_nonnull(record);
  g_assert_cmpstr(record, ==,
      "<CALL:5>K1ABC<QSO_DATE:8>20230415<TIME_ON:6>143000<MODE:4>SSTV<EOR>");
  g_free(record);
}

static void test_reject_invalid_record_fields(void) {
  GError *error = NULL;
  gchar *record = adif_build_sstv_record("", NULL, "20230415", "143000", NULL, &error);
  g_assert_null(record);
  g_assert_nonnull(error);
  g_clear_error(&error);

  record = adif_build_sstv_record("K1ABC", NULL, "2023041x", "143000", NULL, &error);
  g_assert_null(record);
  g_assert_nonnull(error);
  g_clear_error(&error);

  record = adif_build_sstv_record("K1ABC", NULL, "20230415", "14300x", NULL, &error);
  g_assert_null(record);
  g_assert_nonnull(error);
  g_clear_error(&error);

  record = adif_build_sstv_record("K1ABC<EOR>", NULL, "20230415", "143000", NULL, &error);
  g_assert_null(record);
  g_assert_nonnull(error);
  g_clear_error(&error);
}

static void test_send_udp_to_loopback_receiver(void) {
  int receiver;
  struct sockaddr_in address;
  socklen_t address_length = sizeof(address);
  struct timeval timeout = {2, 0};
  const gchar payload[] = "<CALL:5>K1ABC<MODE:4>SSTV<EOR>";
  gchar received[128];
  ssize_t received_length;
  GError *error = NULL;

  receiver = socket(AF_INET, SOCK_DGRAM, 0);
  g_assert_cmpint(receiver, >=, 0);
  memset(&address, 0, sizeof(address));
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  address.sin_port = 0;
  g_assert_cmpint(bind(receiver, (struct sockaddr *)&address, sizeof(address)), ==, 0);
  g_assert_cmpint(getsockname(receiver, (struct sockaddr *)&address, &address_length), ==, 0);
  g_assert_cmpint(setsockopt(receiver, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)), ==, 0);

  g_assert_true(adif_udp_send("127.0.0.1", ntohs(address.sin_port), payload, &error));
  g_assert_no_error(error);
  received_length = recv(receiver, received, sizeof(received), 0);
  g_assert_cmpint(received_length, ==, (ssize_t)strlen(payload));
  g_assert_cmpmem(received, (gsize)received_length, payload, strlen(payload));
  close(receiver);
}

static void test_reject_invalid_udp_destination(void) {
  GError *error = NULL;
  g_assert_false(adif_udp_send("not a valid hostname", 2333, "payload", &error));
  g_assert_nonnull(error);
  g_clear_error(&error);
  g_assert_false(adif_udp_send("127.0.0.1", 0, "payload", &error));
  g_assert_nonnull(error);
  g_clear_error(&error);
}

int main(int argc, char **argv) {
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/adif/build_complete_sstv_record", test_build_complete_sstv_record);
  g_test_add_func("/adif/build_record_without_optional_fields", test_build_record_without_optional_fields);
  g_test_add_func("/adif/reject_invalid_record_fields", test_reject_invalid_record_fields);
  g_test_add_func("/adif/send_udp_to_loopback_receiver", test_send_udp_to_loopback_receiver);
  g_test_add_func("/adif/reject_invalid_udp_destination", test_reject_invalid_udp_destination);
  return g_test_run();
}
