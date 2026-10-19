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

#ifndef PLATFORM_IMPL_APPLE_WIFI_DIRECT_H_
#define PLATFORM_IMPL_APPLE_WIFI_DIRECT_H_

#import <Foundation/Foundation.h>
#import <Network/Network.h>

#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "internal/platform/implementation/wifi_direct.h"
#include "internal/platform/wifi_credential.h"

@class GNCHotspotMedium;
@class GNCNWFrameworkSocket;

namespace nearby {
namespace apple {

class WifiDirectInputStream : public InputStream {
 public:
  explicit WifiDirectInputStream(GNCNWFrameworkSocket* socket);
  ~WifiDirectInputStream() override = default;

  ExceptionOr<ByteArray> Read(std::int64_t size) override;
  Exception Close() override;

 private:
  GNCNWFrameworkSocket* socket_;
};

class WifiDirectOutputStream : public OutputStream {
 public:
  explicit WifiDirectOutputStream(GNCNWFrameworkSocket* socket);
  ~WifiDirectOutputStream() override = default;

  Exception Write(absl::string_view data) override;
  Exception Flush() override;
  Exception Close() override;

 private:
  GNCNWFrameworkSocket* socket_;
};

class WifiDirectSocket : public api::WifiDirectSocket {
 public:
  explicit WifiDirectSocket(GNCNWFrameworkSocket* socket);
  ~WifiDirectSocket() override = default;
  InputStream& GetInputStream() override;
  OutputStream& GetOutputStream() override;
  Exception Close() override;

 private:
  GNCNWFrameworkSocket* const socket_;
  std::unique_ptr<WifiDirectInputStream> input_stream_;
  std::unique_ptr<WifiDirectOutputStream> output_stream_;
};

// macOS cannot create a Wi-Fi Direct group (Group Owner). It can join a group
// created by an Android device and act as a Group Client, using CoreWLAN and
// the same IP socket path as the Wi-Fi Hotspot client.
class WifiDirectMedium : public api::WifiDirectMedium {
 public:
  using WifiDirectAuthType =
      ::location::nearby::proto::connections::WifiDirectAuthType;

  WifiDirectMedium();
  ~WifiDirectMedium() override;

  WifiDirectMedium(const WifiDirectMedium&) = delete;
  WifiDirectMedium& operator=(const WifiDirectMedium&) = delete;

  bool IsInterfaceValid() const override { return true; }

  std::unique_ptr<api::WifiDirectSocket> ConnectToService(
      absl::string_view ip_address, int port,
      CancellationFlag* cancellation_flag) override;

  std::unique_ptr<api::WifiDirectServerSocket> ListenForService(
      int port) override {
    return nullptr;
  }

  bool StartWifiDirect(WifiDirectCredentials* wifi_direct_credentials) override {
    return false;
  }
  bool StopWifiDirect() override { return false; }

  bool ConnectWifiDirect(
      const WifiDirectCredentials& wifi_direct_credentials) override;
  bool DisconnectWifiDirect() override;

  std::optional<std::pair<std::int32_t, std::int32_t>> GetDynamicPortRange()
      override {
    return std::nullopt;
  }

  std::vector<WifiDirectAuthType> GetSupportedWifiDirectAuthTypes()
      const override {
    return {::location::nearby::proto::connections::
                WIFI_DIRECT_WITH_PASSWORD};
  }

 private:
  dispatch_queue_t wifi_direct_queue_ = nil;
  GNCHotspotMedium* medium_;
  NSString* connected_ssid_ = nil;
};

}  // namespace apple
}  // namespace nearby

#endif  // PLATFORM_IMPL_APPLE_WIFI_DIRECT_H_
