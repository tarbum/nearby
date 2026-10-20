// Copyright 2026 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "internal/platform/implementation/apple/wifi_direct.h"

#import <CoreWLAN/CoreWLAN.h>
#import <Foundation/Foundation.h>

#include <arpa/inet.h>
#include <chrono>
#include <ifaddrs.h>
#include <memory>
#include <string>

#include "absl/strings/string_view.h"
#include "internal/base/masker.h"
#include "internal/platform/cancellation_flag.h"
#import "internal/platform/implementation/apple/Log/GNCLogger.h"
#import "internal/platform/implementation/apple/Mediums/Hotspot/GNCHotspotMedium.h"
#import "internal/platform/implementation/apple/Mediums/WiFiCommon/GNCIPv4Address.h"
#import "internal/platform/implementation/apple/Mediums/WiFiCommon/GNCNWFrameworkSocket.h"

namespace nearby {
namespace apple {
namespace {
constexpr char kWifiDirectQueueLabel[] = "com.google.nearby.wifi_direct";

// Returns the first IPv4 address assigned to the Wi-Fi interface, or the
// empty string if none is assigned.
std::string CurrentWifiIPv4() {
  NSString *interface_name = CWWiFiClient.sharedWiFiClient.interface.interfaceName;
  if (interface_name.length == 0) {
    return "";
  }
  std::string result;
  ifaddrs *interfaces = nullptr;
  if (getifaddrs(&interfaces) != 0) {
    return result;
  }
  for (ifaddrs *entry = interfaces; entry != nullptr; entry = entry->ifa_next) {
    if (entry->ifa_addr == nullptr ||
        entry->ifa_addr->sa_family != AF_INET ||
        std::string(entry->ifa_name) != interface_name.UTF8String) {
      continue;
    }
    char address[INET_ADDRSTRLEN] = {0};
    auto *sockaddr_v4 = reinterpret_cast<sockaddr_in *>(entry->ifa_addr);
    if (inet_ntop(AF_INET, &sockaddr_v4->sin_addr, address, sizeof(address)) !=
        nullptr) {
      result = address;
      break;
    }
  }
  freeifaddrs(interfaces);
  return result;
}

// Waits until the Wi-Fi interface acquires an IPv4 lease different from
// `pre_join_ip` (the address held before associating with the group) so that
// callers can immediately dial the group owner's gateway.
void WaitForIpv4Lease(const std::string &pre_join_ip) {
  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (std::chrono::steady_clock::now() < deadline) {
    std::string ip = CurrentWifiIPv4();
    if (!ip.empty() && ip != pre_join_ip) {
      GNCLoggerInfo(@"Wi-Fi Direct interface lease acquired: %s", ip.c_str());
      return;
    }
    [NSThread sleepForTimeInterval:0.25];
  }
  GNCLoggerWarning(@"Timed out waiting for a Wi-Fi Direct interface lease; "
                   "continuing with pre-join address %s",
                   pre_join_ip.c_str());
}
}  // namespace

WifiDirectInputStream::WifiDirectInputStream(GNCNWFrameworkSocket* socket)
    : socket_(socket) {}

ExceptionOr<ByteArray> WifiDirectInputStream::Read(std::int64_t size) {
  NSError* error = nil;
  NSData* data = [socket_ readMaxLength:size error:&error];
  if (data == nil) {
    GNCLoggerError(@"Error reading Wi-Fi Direct socket: %@", error);
    return {Exception::kIo};
  }
  return ExceptionOr<ByteArray>{ByteArray((const char*)data.bytes, data.length)};
}

Exception WifiDirectInputStream::Close() { return {Exception::kSuccess}; }

WifiDirectOutputStream::WifiDirectOutputStream(GNCNWFrameworkSocket* socket)
    : socket_(socket) {}

Exception WifiDirectOutputStream::Write(absl::string_view data) {
  NSError* error = nil;
  BOOL result = [socket_ write:[NSData dataWithBytes:data.data() length:data.size()]
                         error:&error];
  if (!result) {
    GNCLoggerError(@"Error writing Wi-Fi Direct socket: %@", error);
    return {Exception::kIo};
  }
  return {Exception::kSuccess};
}

Exception WifiDirectOutputStream::Flush() { return {Exception::kSuccess}; }

Exception WifiDirectOutputStream::Close() { return {Exception::kSuccess}; }

WifiDirectSocket::WifiDirectSocket(GNCNWFrameworkSocket* socket)
    : socket_(socket),
      input_stream_(std::make_unique<WifiDirectInputStream>(socket)),
      output_stream_(std::make_unique<WifiDirectOutputStream>(socket)) {}

InputStream& WifiDirectSocket::GetInputStream() { return *input_stream_; }

OutputStream& WifiDirectSocket::GetOutputStream() { return *output_stream_; }

Exception WifiDirectSocket::Close() {
  [socket_ close];
  return {Exception::kSuccess};
}

WifiDirectMedium::WifiDirectMedium() {
  wifi_direct_queue_ =
      dispatch_queue_create(kWifiDirectQueueLabel, DISPATCH_QUEUE_SERIAL);
  medium_ = [[GNCHotspotMedium alloc] initWithQueue:wifi_direct_queue_];
}

WifiDirectMedium::~WifiDirectMedium() { DisconnectWifiDirect(); }

bool WifiDirectMedium::ConnectWifiDirect(
    const WifiDirectCredentials& credentials) {
  std::string ssid = credentials.GetSSID();
  std::string password = credentials.GetPassword();
  if (ssid.empty() || password.empty()) {
    // The device-name/PIN flow is used by Windows; macOS would need a WPS
    // implementation. Android's SSID/password path is what we support.
    GNCLoggerError(@"Wi-Fi Direct credentials missing SSID/password");
    return false;
  }
  NSString* ns_ssid = [[NSString alloc] initWithUTF8String:ssid.c_str()];
  NSString* ns_password = [[NSString alloc] initWithUTF8String:password.c_str()];
  GNCLoggerInfo(@"ConnectWifiDirect SSID: %@ Password: %s", ns_ssid,
                masker::Mask(password).c_str());
  std::string pre_join_ip = CurrentWifiIPv4();
  if (![medium_ connectToWifiNetworkWithSSID:ns_ssid password:ns_password]) {
    GNCLoggerError(@"Failed to join Wi-Fi Direct group %@", ns_ssid);
    return false;
  }
  connected_ssid_ = ns_ssid;
  // The BWU handler dials the Group Owner's gateway immediately after this
  // returns. Association alone doesn't guarantee connectivity; wait for DHCP
  // to assign the group address (e.g. 192.168.49.x) or the first socket
  // connect fails with no route.
  WaitForIpv4Lease(pre_join_ip);
  return true;
}

bool WifiDirectMedium::DisconnectWifiDirect() {
  if (connected_ssid_) {
    [medium_ disconnectToWifiNetworkWithSSID:connected_ssid_];
    connected_ssid_ = nil;
  }
  return true;
}

std::unique_ptr<api::WifiDirectSocket> WifiDirectMedium::ConnectToService(
    absl::string_view ip_address, int port,
    CancellationFlag* cancellation_flag) {
  NSError* error = nil;
  if (ip_address.size() != 4) {
    GNCLoggerError(@"Invalid Wi-Fi Direct IPv4 address size: %zu",
                   ip_address.size());
    return nullptr;
  }
  NSData* host_ip_address = [NSData dataWithBytes:ip_address.data()
                                           length:ip_address.size()];
  GNCIPv4Address* host = [GNCIPv4Address addressFromData:host_ip_address];
  GNCLoggerInfo(@"Connect to Wi-Fi Direct host: %@ port %d",
                [host dottedRepresentation], port);

  if (cancellation_flag != nullptr && cancellation_flag->Cancelled()) {
    return nullptr;
  }

  GNCNWFrameworkSocket* socket = [medium_ connectToHost:host
                                                   port:port
                                           cancelSource:nullptr
                                                  error:&error];
  if (socket == nil) {
    if (error != nil) {
      GNCLoggerError(@"Error connecting to Wi-Fi Direct host %@:%d: %@", host,
                     port, error);
    }
    return nullptr;
  }
  return std::make_unique<WifiDirectSocket>(socket);
}

}  // namespace apple
}  // namespace nearby
