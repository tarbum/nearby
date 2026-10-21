#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

#include "nc.h"

namespace {

void OnResult(NC_STATUS status, CALLER_CONTEXT) {
  std::printf("result: %d\n", static_cast<int>(status));
  std::fflush(stdout);
}

void OnInitiated(NC_INSTANCE, int endpoint_id,
                 const NC_CONNECTION_RESPONSE_INFO*, CALLER_CONTEXT) {
  std::printf("connection initiated: %d\n", endpoint_id);
  std::fflush(stdout);
}

void OnAccepted(NC_INSTANCE, int endpoint_id, CALLER_CONTEXT) {
  std::printf("connection accepted: %d\n", endpoint_id);
  std::fflush(stdout);
}

void OnRejected(NC_INSTANCE, int endpoint_id, NC_STATUS status, CALLER_CONTEXT) {
  std::printf("connection rejected: %d (%d)\n", endpoint_id,
              static_cast<int>(status));
  std::fflush(stdout);
}

void OnDisconnected(NC_INSTANCE, int endpoint_id, CALLER_CONTEXT) {
  std::printf("connection disconnected: %d\n", endpoint_id);
  std::fflush(stdout);
}

void OnBandwidthChanged(NC_INSTANCE, int endpoint_id, NC_MEDIUM medium,
                        CALLER_CONTEXT) {
  std::printf("bandwidth changed: %d medium=%d\n", endpoint_id,
              static_cast<int>(medium));
  std::fflush(stdout);
}

void OnEndpointFound(NC_INSTANCE, int endpoint_id, const NC_DATA* endpoint_info,
                     const NC_DATA* service_id, CALLER_CONTEXT) {
  std::string info;
  if (endpoint_info != nullptr && endpoint_info->data != nullptr) {
    info.assign(endpoint_info->data, endpoint_info->size);
  }
  std::printf("endpoint found: %d info=%zu bytes\n", endpoint_id, info.size());
  std::fflush(stdout);
}

void OnEndpointLost(NC_INSTANCE, int endpoint_id, CALLER_CONTEXT) {
  std::printf("endpoint lost: %d\n", endpoint_id);
  std::fflush(stdout);
}

void OnEndpointDistanceChanged(NC_INSTANCE, int endpoint_id,
                               NC_DISTANCE_INFO distance, CALLER_CONTEXT) {
  std::printf("endpoint distance: %d -> %d\n", endpoint_id,
              static_cast<int>(distance));
  std::fflush(stdout);
}

}  // namespace

int main() {
  NC_INSTANCE instance = NcCreateService();
  if (instance == nullptr) {
    std::fprintf(stderr, "could not create service\n");
    return 1;
  }

  std::string service_id = "NearbySharing";
  NC_DATA service{service_id.size(), service_id.data()};

  NC_ADVERTISING_OPTIONS options{};
  options.common_options.strategy.type = NC_STRATEGY_TYPE_P2P_POINT_TO_POINT;
  options.common_options.strategy.connection_type =
      NC_CONNECTION_TYPE_POINT_TO_POINT;
  options.common_options.strategy.topology_type = NC_TOPOLOGY_TYPE_ONE_TO_ONE;
  options.common_options.allowed_mediums[NC_MEDIUM_WIFI_LAN] = true;
  options.auto_upgrade_bandwidth = false;
  options.enforce_topology_constraints = false;
  options.low_power = false;

  std::string endpoint_info = "linux-smoke";
  NC_CONNECTION_REQUEST_INFO request{};
  request.endpoint_info = {endpoint_info.size(), endpoint_info.data()};
  request.initiated_callback = OnInitiated;
  request.accepted_callback = OnAccepted;
  request.rejected_callback = OnRejected;
  request.disconnected_callback = OnDisconnected;
  request.bandwidth_changed_callback = OnBandwidthChanged;

  std::printf("starting advertising\n");
  std::fflush(stdout);
  NcStartAdvertising(instance, &service, &options, &request, OnResult, nullptr);

  NC_DISCOVERY_OPTIONS discovery_options{};
  discovery_options.common_options.strategy.type =
      NC_STRATEGY_TYPE_P2P_POINT_TO_POINT;
  discovery_options.common_options.strategy.connection_type =
      NC_CONNECTION_TYPE_POINT_TO_POINT;
  discovery_options.common_options.strategy.topology_type =
      NC_TOPOLOGY_TYPE_ONE_TO_ONE;
  discovery_options.common_options.allowed_mediums[NC_MEDIUM_WIFI_LAN] = true;
  NC_DISCOVERY_LISTENER discovery_listener{};
  discovery_listener.endpoint_found_callback = OnEndpointFound;
  discovery_listener.endpoint_lost_callback = OnEndpointLost;
  discovery_listener.endpoint_distance_changed_callback =
      OnEndpointDistanceChanged;
  NcStartDiscovery(instance, &service, &discovery_options, &discovery_listener,
                   OnResult, nullptr);

  std::this_thread::sleep_for(std::chrono::seconds(15));
  NcStopAdvertising(instance, OnResult, nullptr);
  NcCloseService(instance);
  return 0;
}
