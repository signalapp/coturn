#include "prom_server.h"
#include "mainrelay.h"
#include "ns_turn_utils.h"
#if !defined(WINDOWS)
#include <errno.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#if !defined(TURN_NO_PROMETHEUS)

#define PROTOCOL_GROUP_LABEL "protocol-group"

prom_counter_t *stun_binding_request;
prom_counter_t *stun_binding_response;
prom_counter_t *stun_binding_error;

prom_counter_t *turn_traffic_rcvp;
prom_counter_t *turn_traffic_rcvb;
prom_counter_t *turn_traffic_sentp;
prom_counter_t *turn_traffic_sentb;

prom_counter_t *turn_traffic_peer_rcvp;
prom_counter_t *turn_traffic_peer_rcvb;
prom_counter_t *turn_traffic_peer_sentp;
prom_counter_t *turn_traffic_peer_sentb;

prom_counter_t *turn_total_traffic_rcvp;
prom_counter_t *turn_total_traffic_rcvb;
prom_counter_t *turn_total_traffic_sentp;
prom_counter_t *turn_total_traffic_sentb;

prom_counter_t *turn_total_traffic_peer_rcvp;
prom_counter_t *turn_total_traffic_peer_rcvb;
prom_counter_t *turn_total_traffic_peer_sentp;
prom_counter_t *turn_total_traffic_peer_sentb;

prom_counter_t *turn_total_sessions;

prom_gauge_t *turn_total_allocations;

// Signal change to add metrics
prom_counter_t *turn_rtt_client[8];
prom_counter_t *turn_rtt_peer[8];
prom_counter_t *turn_rtt_combined[8];
prom_counter_t *turn_with_no_ping_rcvp;
prom_counter_t *turn_allocation_response;
prom_gauge_t *turn_session_limit;
prom_counter_t *turn_sessions_overlimit;

#if MHD_VERSION >= 0x00097002
#define MHD_RESULT enum MHD_Result
#else
#define MHD_RESULT int
#endif

MHD_RESULT promhttp_handler(void *cls, struct MHD_Connection *connection, const char *url, const char *method,
                            const char *version, const char *upload_data, size_t *upload_data_size, void **con_cls) {
  MHD_RESULT ret;

  char *body = "not found";
  enum MHD_ResponseMemoryMode mode = MHD_RESPMEM_PERSISTENT;
  unsigned int status = MHD_HTTP_NOT_FOUND;

  if (strcmp(method, "GET") != 0) {
    status = MHD_HTTP_METHOD_NOT_ALLOWED;
    body = "method not allowed";
  } else if (strcmp(url, turn_params.prometheus_path) == 0) {
    body = prom_collector_registry_bridge(PROM_COLLECTOR_REGISTRY_DEFAULT);
    mode = MHD_RESPMEM_MUST_FREE;
    status = MHD_HTTP_OK;
  }

  struct MHD_Response *response = MHD_create_response_from_buffer(strlen(body), body, mode);
  if (response == NULL) {
    if (mode == MHD_RESPMEM_MUST_FREE) {
      free(body);
    }
    ret = MHD_NO;
  } else {
    ret = MHD_queue_response(connection, status, response);
    MHD_destroy_response(response);
  }
  return ret;
}

void start_prometheus_server(void) {
  if (turn_params.prometheus == 0) {
    TURN_LOG_FUNC(TURN_LOG_LEVEL_INFO, "prometheus collector disabled, not started\n");
    return;
  }
  prom_collector_registry_default_init();

  // Signal change to add protocol-group label to metrics
  const char *label[] = {"realm", PROTOCOL_GROUP_LABEL, PROTOCOL_GROUP_LABEL};
  size_t nlabels = 2;

  if (turn_params.prometheus_username_labels) {
    label[1] = "user";
    nlabels++;
  }

  // Create STUN counters
  stun_binding_request = prom_collector_registry_must_register_metric(
      prom_counter_new("stun_binding_request", "Incoming STUN Binding requests", 0, NULL));
  stun_binding_response = prom_collector_registry_must_register_metric(
      prom_counter_new("stun_binding_response", "Outgoing STUN Binding responses", 0, NULL));
  stun_binding_error = prom_collector_registry_must_register_metric(
      prom_counter_new("stun_binding_error", "STUN Binding errors", 0, NULL));

  // Create TURN traffic counter metrics
  turn_traffic_rcvp = prom_collector_registry_must_register_metric(
      prom_counter_new("turn_traffic_rcvp", "Represents finished sessions received packets", nlabels, label));
  turn_traffic_rcvb = prom_collector_registry_must_register_metric(
      prom_counter_new("turn_traffic_rcvb", "Represents finished sessions received bytes", nlabels, label));
  turn_traffic_sentp = prom_collector_registry_must_register_metric(
      prom_counter_new("turn_traffic_sentp", "Represents finished sessions sent packets", nlabels, label));
  turn_traffic_sentb = prom_collector_registry_must_register_metric(
      prom_counter_new("turn_traffic_sentb", "Represents finished sessions sent bytes", nlabels, label));

  // Create finished sessions traffic for peers counter metrics
  turn_traffic_peer_rcvp = prom_collector_registry_must_register_metric(
      prom_counter_new("turn_traffic_peer_rcvp", "Represents finished sessions peer received packets", nlabels, label));
  turn_traffic_peer_rcvb = prom_collector_registry_must_register_metric(
      prom_counter_new("turn_traffic_peer_rcvb", "Represents finished sessions peer received bytes", nlabels, label));
  turn_traffic_peer_sentp = prom_collector_registry_must_register_metric(
      prom_counter_new("turn_traffic_peer_sentp", "Represents finished sessions peer sent packets", nlabels, label));
  turn_traffic_peer_sentb = prom_collector_registry_must_register_metric(
      prom_counter_new("turn_traffic_peer_sentb", "Represents finished sessions peer sent bytes", nlabels, label));

  const char *traffic_label[] = {PROTOCOL_GROUP_LABEL};
  const size_t ntraffic_labels = 1;
  // Create total finished traffic counter metrics
  turn_total_traffic_rcvp = prom_collector_registry_must_register_metric(
      prom_counter_new("turn_total_traffic_rcvp", "Represents total finished sessions received packets",
                       ntraffic_labels, traffic_label));
  turn_total_traffic_rcvb = prom_collector_registry_must_register_metric(prom_counter_new(
      "turn_total_traffic_rcvb", "Represents total finished sessions received bytes", ntraffic_labels, traffic_label));
  turn_total_traffic_sentp = prom_collector_registry_must_register_metric(prom_counter_new(
      "turn_total_traffic_sentp", "Represents total finished sessions sent packets", ntraffic_labels, traffic_label));
  turn_total_traffic_sentb = prom_collector_registry_must_register_metric(prom_counter_new(
      "turn_total_traffic_sentb", "Represents total finished sessions sent bytes", ntraffic_labels, traffic_label));

  // Create total finished sessions traffic for peers counter metrics
  turn_total_traffic_peer_rcvp = prom_collector_registry_must_register_metric(
      prom_counter_new("turn_total_traffic_peer_rcvp", "Represents total finished sessions peer received packets",
                       ntraffic_labels, traffic_label));
  turn_total_traffic_peer_rcvb = prom_collector_registry_must_register_metric(
      prom_counter_new("turn_total_traffic_peer_rcvb", "Represents total finished sessions peer received bytes",
                       ntraffic_labels, traffic_label));
  turn_total_traffic_peer_sentp = prom_collector_registry_must_register_metric(
      prom_counter_new("turn_total_traffic_peer_sentp", "Represents total finished sessions peer sent packets",
                       ntraffic_labels, traffic_label));
  turn_total_traffic_peer_sentb = prom_collector_registry_must_register_metric(
      prom_counter_new("turn_total_traffic_peer_sentb", "Represents total finished sessions peer sent bytes",
                       ntraffic_labels, traffic_label));

  // Signal change to add protocol-group metric label
  // Create total completed session counter metric
  const char *total_sessions_labels[] = {"duration", "sent_rate", PROTOCOL_GROUP_LABEL};
  turn_total_sessions = prom_collector_registry_must_register_metric(
      prom_counter_new("turn_total_sessions", "Represents total completed sessions", 3, total_sessions_labels));

  // Create total allocations number gauge metric
  const char *total_allocations_labels[] = {"type", "client_addr_family", PROTOCOL_GROUP_LABEL};
  turn_total_allocations = prom_collector_registry_must_register_metric(
      prom_gauge_new("turn_total_allocations", "Represents current allocations number", 3, total_allocations_labels));

  // Signal change to add metrics
  // Create round trip time pseudo-histogram metrics
  // values must be kept in sync with observation function below

  const char *rtt_labels[] = {PROTOCOL_GROUP_LABEL};
  const size_t nrtt_labels = 1;
  turn_rtt_client[0] = prom_collector_registry_must_register_metric(
      prom_counter_new("turn_rtt_client_le_25ms", "Represents measured round trip time of client with channel",
                       nrtt_labels, rtt_labels));
  turn_rtt_client[1] = prom_collector_registry_must_register_metric(
      prom_counter_new("turn_rtt_client_le_50ms", "Represents measured round trip time of client with channel",
                       nrtt_labels, rtt_labels));
  turn_rtt_client[2] = prom_collector_registry_must_register_metric(
      prom_counter_new("turn_rtt_client_le_100ms", "Represents measured round trip time of client with channel",
                       nrtt_labels, rtt_labels));
  turn_rtt_client[3] = prom_collector_registry_must_register_metric(
      prom_counter_new("turn_rtt_client_le_200ms", "Represents measured round trip time of client with channel",
                       nrtt_labels, rtt_labels));
  turn_rtt_client[4] = prom_collector_registry_must_register_metric(
      prom_counter_new("turn_rtt_client_le_400ms", "Represents measured round trip time of client with channel",
                       nrtt_labels, rtt_labels));
  turn_rtt_client[5] = prom_collector_registry_must_register_metric(
      prom_counter_new("turn_rtt_client_le_800ms", "Represents measured round trip time of client with channel",
                       nrtt_labels, rtt_labels));
  turn_rtt_client[6] = prom_collector_registry_must_register_metric(
      prom_counter_new("turn_rtt_client_le_1500ms", "Represents measured round trip time of client with channel",
                       nrtt_labels, rtt_labels));
  turn_rtt_client[7] = prom_collector_registry_must_register_metric(prom_counter_new(
      "turn_rtt_client_more", "Represents measured round trip time of client with channel", nrtt_labels, rtt_labels));

  turn_rtt_peer[0] = prom_collector_registry_must_register_metric(prom_counter_new(
      "turn_rtt_peer_le_25ms", "Represents measured round trip time of peer with channel", nrtt_labels, rtt_labels));
  turn_rtt_peer[1] = prom_collector_registry_must_register_metric(prom_counter_new(
      "turn_rtt_peer_le_50ms", "Represents measured round trip time of peer with channel", nrtt_labels, rtt_labels));
  turn_rtt_peer[2] = prom_collector_registry_must_register_metric(prom_counter_new(
      "turn_rtt_peer_le_100ms", "Represents measured round trip time of peer with channel", nrtt_labels, rtt_labels));
  turn_rtt_peer[3] = prom_collector_registry_must_register_metric(prom_counter_new(
      "turn_rtt_peer_le_200ms", "Represents measured round trip time of peer with channel", nrtt_labels, rtt_labels));
  turn_rtt_peer[4] = prom_collector_registry_must_register_metric(prom_counter_new(
      "turn_rtt_peer_le_400ms", "Represents measured round trip time of peer with channel", nrtt_labels, rtt_labels));
  turn_rtt_peer[5] = prom_collector_registry_must_register_metric(prom_counter_new(
      "turn_rtt_peer_le_800ms", "Represents measured round trip time of peer with channel", nrtt_labels, rtt_labels));
  turn_rtt_peer[6] = prom_collector_registry_must_register_metric(prom_counter_new(
      "turn_rtt_peer_le_1500ms", "Represents measured round trip time of peer with channel", nrtt_labels, rtt_labels));
  turn_rtt_peer[7] = prom_collector_registry_must_register_metric(prom_counter_new(
      "turn_rtt_peer_more", "Represents measured round trip time of peer with channel", nrtt_labels, rtt_labels));

  turn_rtt_combined[0] = prom_collector_registry_must_register_metric(prom_counter_new(
      "turn_rtt_combined_le_25ms", "Represents combined round trip time of channel", nrtt_labels, rtt_labels));
  turn_rtt_combined[1] = prom_collector_registry_must_register_metric(prom_counter_new(
      "turn_rtt_combined_le_50ms", "Represents combined round trip time of channel", nrtt_labels, rtt_labels));
  turn_rtt_combined[2] = prom_collector_registry_must_register_metric(prom_counter_new(
      "turn_rtt_combined_le_100ms", "Represents combined round trip time of channel", nrtt_labels, rtt_labels));
  turn_rtt_combined[3] = prom_collector_registry_must_register_metric(prom_counter_new(
      "turn_rtt_combined_le_200ms", "Represents combined round trip time of channel", nrtt_labels, rtt_labels));
  turn_rtt_combined[4] = prom_collector_registry_must_register_metric(prom_counter_new(
      "turn_rtt_combined_le_400ms", "Represents combined round trip time of channel", nrtt_labels, rtt_labels));
  turn_rtt_combined[5] = prom_collector_registry_must_register_metric(prom_counter_new(
      "turn_rtt_combined_le_800ms", "Represents combined round trip time of channel", nrtt_labels, rtt_labels));
  turn_rtt_combined[6] = prom_collector_registry_must_register_metric(prom_counter_new(
      "turn_rtt_combined_le_1500ms", "Represents combined round trip time of channel", nrtt_labels, rtt_labels));
  turn_rtt_combined[7] = prom_collector_registry_must_register_metric(prom_counter_new(
      "turn_rtt_combined_more", "Represents combined round trip time of channel", nrtt_labels, rtt_labels));

  turn_with_no_ping_rcvp = prom_collector_registry_must_register_metric(prom_counter_new(
      "turn_with_no_ping_rcvp", "Count of packets received for TURN where no ICE ping has been observed", 0, NULL));

  const char *response_labels[] = {"response_code"};
  turn_allocation_response = prom_collector_registry_must_register_metric(prom_counter_new(
      "turn_allocation_response", "Count of allocation responses sent (by error code)", 1, response_labels));

  turn_session_limit = prom_collector_registry_must_register_metric(
      prom_gauge_new("turn_session_limit", "Current number of additional sessions allowed", 0, NULL));

  turn_sessions_overlimit = prom_collector_registry_must_register_metric(prom_counter_new(
      "turn_sessions_overlimit", "Count of sessions deined because it would be over the limit", 0, NULL));

  // some flags appeared first in microhttpd v0.9.53
  unsigned int flags = 0;
#if MHD_VERSION >= 0x00095300
  flags |= MHD_USE_ERROR_LOG;
#endif
  if (MHD_is_feature_supported(MHD_FEATURE_EPOLL)) {
#if MHD_VERSION >= 0x00095300
    flags |= MHD_USE_EPOLL_INTERNAL_THREAD;
#else
    flags |= MHD_USE_EPOLL_INTERNALLY_LINUX_ONLY; // old versions of microhttpd
#endif
    TURN_LOG_FUNC(TURN_LOG_LEVEL_INFO, "prometheus exporter server will start using EPOLL\n");
  } else {
    flags |= MHD_USE_SELECT_INTERNALLY;
    // Select() will not work if all 1024 first file-descriptors are used.
    // In this case the prometheus server will be unreachable
    TURN_LOG_FUNC(TURN_LOG_LEVEL_WARNING, "prometheus exporter server will start using SELECT. "
                                          "The exporter might be unreachable on highly used servers\n");
  }

  ioa_addr server_addr;
  addr_set_any(&server_addr);
  if (turn_params.prometheus_address[0]) {
    if (make_ioa_addr((const uint8_t *)turn_params.prometheus_address, turn_params.prometheus_port, &server_addr) < 0) {
      TURN_LOG_FUNC(TURN_LOG_LEVEL_ERROR, "could not parse prometheus collector's server address\n");
      return;
    }

    if (is_ipv6_enabled() && server_addr.ss.sa_family == AF_INET6) {
      flags |= MHD_USE_IPv6;
    }
  } else {
    if (MHD_is_feature_supported(MHD_FEATURE_IPv6) && is_ipv6_enabled()) {
      flags |= MHD_USE_DUAL_STACK;
      server_addr.ss.sa_family = AF_INET6;
      server_addr.s6.sin6_port = htons((uint16_t)turn_params.prometheus_port);
    } else {
      server_addr.ss.sa_family = AF_INET;
      server_addr.s4.sin_port = htons((uint16_t)turn_params.prometheus_port);
    }
  }

  uint8_t addr[MAX_IOA_ADDR_STRING];
  addr_to_string(&server_addr, addr);
  TURN_LOG_FUNC(TURN_LOG_LEVEL_INFO, "prometheus exporter server will listen on %s\n", addr);

  struct MHD_Daemon *daemon =
      MHD_start_daemon(flags, 0, NULL, NULL, &promhttp_handler, NULL, MHD_OPTION_LISTENING_ADDRESS_REUSE, 1,
                       MHD_OPTION_SOCK_ADDR, &server_addr, MHD_OPTION_END);
  if (daemon == NULL) {
    TURN_LOG_FUNC(TURN_LOG_LEVEL_ERROR, "could not start prometheus collector\n");
    return;
  }

  TURN_LOG_FUNC(TURN_LOG_LEVEL_INFO, "prometheus collector started successfully\n");

  return;
}

// Signal change to add metrics
void prom_set_finished_traffic(const char *realm, const char *user, unsigned long rsvp, unsigned long rsvb,
                               unsigned long sentp, unsigned long sentb, unsigned long without_pingp, bool peer,
                               const char *protocolgroup) {
  if (turn_params.prometheus == 1) {

    // Signal change to add protocol-group label to metrics
    const char *user_label[] = {realm, protocolgroup, protocolgroup};
    if (turn_params.prometheus_username_labels) {
      user_label[1] = user;
    }
    const char *traffic_label[] = {protocolgroup};
    // end signal change

    if (peer) {
      prom_counter_add(turn_traffic_peer_rcvp, rsvp, user_label);
      prom_counter_add(turn_traffic_peer_rcvb, rsvb, user_label);
      prom_counter_add(turn_traffic_peer_sentp, sentp, user_label);
      prom_counter_add(turn_traffic_peer_sentb, sentb, user_label);

      prom_counter_add(turn_total_traffic_peer_rcvp, rsvp, traffic_label);
      prom_counter_add(turn_total_traffic_peer_rcvb, rsvb, traffic_label);
      prom_counter_add(turn_total_traffic_peer_sentp, sentp, traffic_label);
      prom_counter_add(turn_total_traffic_peer_sentb, sentb, traffic_label);
    } else {
      prom_counter_add(turn_traffic_rcvp, rsvp, user_label);
      prom_counter_add(turn_traffic_rcvb, rsvb, user_label);
      prom_counter_add(turn_traffic_sentp, sentp, user_label);
      prom_counter_add(turn_traffic_sentb, sentb, user_label);

      prom_counter_add(turn_total_traffic_rcvp, rsvp, traffic_label);
      prom_counter_add(turn_total_traffic_rcvb, rsvb, traffic_label);
      prom_counter_add(turn_total_traffic_sentp, sentp, traffic_label);
      prom_counter_add(turn_total_traffic_sentb, sentb, traffic_label);
    }
    // Signal change to add metrics
    if (without_pingp) {
      prom_counter_add(turn_with_no_ping_rcvp, without_pingp, NULL);
    }
  }
}

void prom_inc_allocation(SOCKET_TYPE type, int addr_family, const char *protocolgroup) {
  if (turn_params.prometheus == 1) {
    const char *labels[] = {socket_type_name(type), addr_family_name(addr_family), protocolgroup};
    prom_gauge_inc(turn_total_allocations, labels);
  }
}

void prom_dec_allocation(SOCKET_TYPE type, int addr_family, unsigned long duration, unsigned long sent_rate_kbps,
                         const char *protocolgroup) {
  if (turn_params.prometheus == 1) {
    const char *labels[] = {socket_type_name(type), addr_family_name(addr_family), protocolgroup};
    prom_gauge_dec(turn_total_allocations, labels);
    const char *total_sessions_labels[] = {duration_name(duration), rate_name(sent_rate_kbps), protocolgroup};
    prom_counter_add(turn_total_sessions, 1, total_sessions_labels);
  }
}

void prom_inc_stun_binding_request(void) {
  if (turn_params.prometheus == 1) {
    prom_counter_add(stun_binding_request, 1, NULL);
  }
}

void prom_inc_stun_binding_response(void) {
  if (turn_params.prometheus == 1) {
    prom_counter_add(stun_binding_response, 1, NULL);
  }
}

void prom_inc_stun_binding_error(void) {
  if (turn_params.prometheus == 1) {
    prom_counter_add(stun_binding_error, 1, NULL);
  }
}

int is_ipv6_enabled(void) {
  int ret = 0;

#ifdef AF_INET6
  int fd = socket(AF_INET6, SOCK_STREAM, 0);
  if (fd == -1) {
    ret = errno != EAFNOSUPPORT;
  } else {
    ret = 1;
    close(fd);
  }
#endif /* AF_INET6 */

  return ret;
}

// Signal change to add metrics
void prom_observe_rtt(prom_counter_t *counter[8], int microseconds, const char *protocolgroup) {
  const char *label[] = {protocolgroup};
  if (microseconds <= 25000) {
    prom_counter_add(counter[0], 1, label);
  }
  if (microseconds <= 50000) {
    prom_counter_add(counter[1], 1, label);
  }
  if (microseconds <= 100000) {
    prom_counter_add(counter[2], 1, label);
  }
  if (microseconds <= 200000) {
    prom_counter_add(counter[3], 1, label);
  }
  if (microseconds <= 400000) {
    prom_counter_add(counter[4], 1, label);
  }
  if (microseconds <= 800000) {
    prom_counter_add(counter[5], 1, label);
  }
  if (microseconds <= 1500000) {
    prom_counter_add(counter[6], 1, label);
  }
  prom_counter_add(counter[7], 1, label);
}

void prom_observe_rtt_client(int microseconds, const char *protocolgroup) {
  if (turn_params.prometheus == 1) {
    prom_observe_rtt(turn_rtt_client, microseconds, protocolgroup);
  }
}

void prom_observe_rtt_peer(int microseconds, const char *protocolgroup) {
  if (turn_params.prometheus == 1) {
    prom_observe_rtt(turn_rtt_peer, microseconds, protocolgroup);
  }
}

void prom_observe_rtt_combined(int microseconds, const char *protocolgroup) {
  if (turn_params.prometheus == 1) {
    prom_observe_rtt(turn_rtt_combined, microseconds, protocolgroup);
  }
}

void prom_inc_allocation_response(int err_code) {
  if (turn_params.prometheus == 1) {
    char label[80];
    if (snprintf(label, sizeof(label), "%d", err_code) < (int)sizeof(label)) {
      const char *labels[] = {label};
      prom_counter_add(turn_allocation_response, 1, labels);
    }
  }
}

void prom_set_session_limit(int limit) {
  if (turn_params.prometheus == 1) {
    prom_gauge_set(turn_session_limit, limit, NULL);
  }
}

void prom_inc_sessions_overlimit(void) {
  if (turn_params.prometheus == 1) {
    prom_counter_add(turn_sessions_overlimit, 1, NULL);
  }
}

#else

void start_prometheus_server(void) {
  TURN_LOG_FUNC(TURN_LOG_LEVEL_INFO, "turnserver compiled without prometheus support\n");
  return;
}

void prom_set_finished_traffic(const char *realm, const char *user, unsigned long rsvp, unsigned long rsvb,
                               unsigned long sentp, unsigned long sentb, unsigned long without_pingp, bool peer,
                               const char *protocolgroup) {
  UNUSED_ARG(realm);
  UNUSED_ARG(user);
  UNUSED_ARG(rsvp);
  UNUSED_ARG(rsvb);
  UNUSED_ARG(sentp);
  UNUSED_ARG(sentb);
  UNUSED_ARG(without_pingp);
  UNUSED_ARG(peer);
  UNUSED_ARG(protocolgroup);
}

void prom_inc_allocation(SOCKET_TYPE type, int addr_family, const char *protocolgroup) {
  UNUSED_ARG(type);
  UNUSED_ARG(addr_family);
  UNUSED_ARG(protocolgroup);
}

void prom_dec_allocation(SOCKET_TYPE type, int addr_family, unsigned long duration, unsigned long sent_rate_kbps,
                         const char *protocolgroup) {
  UNUSED_ARG(type);
  UNUSED_ARG(addr_family);
  UNUSED_ARG(duration);
  UNUSED_ARG(sent_rate_kbps);
  UNUSED_ARG(protocolgroup);
}

#endif /* TURN_NO_PROMETHEUS */
