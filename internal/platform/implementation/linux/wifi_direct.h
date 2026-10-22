#ifndef THIRD_PARTY_NEARBY_INTERNAL_PLATFORM_IMPLEMENTATION_LINUX_WIFI_DIRECT_H_
#define THIRD_PARTY_NEARBY_INTERNAL_PLATFORM_IMPLEMENTATION_LINUX_WIFI_DIRECT_H_

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "absl/strings/string_view.h"
#include "internal/platform/cancellation_flag.h"
#include "internal/platform/implementation/wifi_direct.h"
#include "internal/platform/input_stream.h"
#include "internal/platform/output_stream.h"
#include "internal/platform/wifi_credential.h"

namespace nearby {
namespace linux_platform {

class WifiDirectMediumImpl;

class WifiDirectSocketImpl final : public api::WifiDirectSocket {
 public:
  explicit WifiDirectSocketImpl(int fd);
  ~WifiDirectSocketImpl() override;
  InputStream& GetInputStream() override { return *input_; }
  OutputStream& GetOutputStream() override { return *output_; }
  Exception Close() override;

 private:
  class SocketInputStream;
  class SocketOutputStream;

  int fd_;
  std::unique_ptr<InputStream> input_;
  std::unique_ptr<OutputStream> output_;
  bool closed_ = false;
};

class WifiDirectServerSocketImpl final : public api::WifiDirectServerSocket {
 public:
  explicit WifiDirectServerSocketImpl(WifiDirectMediumImpl* medium, int fd,
                                      int port);
  ~WifiDirectServerSocketImpl() override;
  std::string GetIPAddress() const override;
  int GetPort() const override { return port_; }
  std::unique_ptr<api::WifiDirectSocket> Accept() override;
  Exception Close() override;
  void PopulateWifiDirectCredentials(
      WifiDirectCredentials& wifi_direct_credentials) override;

 private:
  WifiDirectMediumImpl* medium_;
  int fd_;
  int port_;
  bool closed_ = false;
};

class WifiDirectMediumImpl final : public api::WifiDirectMedium {
 public:
  WifiDirectMediumImpl() = default;
  ~WifiDirectMediumImpl() override;

  bool IsInterfaceValid() const override;
  std::unique_ptr<api::WifiDirectSocket> ConnectToService(
      absl::string_view ip_address, int port,
      CancellationFlag* cancellation_flag) override;
  std::unique_ptr<api::WifiDirectServerSocket> ListenForService(
      int port) override;
  bool StartWifiDirect(WifiDirectCredentials* wifi_direct_credentials) override;
  bool StopWifiDirect() override;
  bool ConnectWifiDirect(
      const WifiDirectCredentials& wifi_direct_credentials) override;
  bool DisconnectWifiDirect() override;
  std::optional<std::pair<std::int32_t, std::int32_t>> GetDynamicPortRange()
      override {
    return std::make_pair(1024, 65535);
  }
  std::vector<WifiDirectAuthType> GetSupportedWifiDirectAuthTypes()
      const override {
    return {WifiDirectAuthType::WIFI_DIRECT_WITH_PASSWORD};
  }

  std::string GetWlanIPv4() const;

 private:
  enum class Mode { kNone, kHotspot, kP2p };

  bool StartP2p(WifiDirectCredentials* wifi_direct_credentials);
  bool StartHotspot(WifiDirectCredentials* wifi_direct_credentials);
  void StopP2p();

  std::string ssid_;
  std::string password_;
  std::string gateway_;
  std::string joined_ssid_;
  std::string joined_profile_;
  std::string previous_wifi_;
  std::vector<std::string> disabled_profiles_;
  Mode mode_ = Mode::kNone;
  std::string p2p_interface_;
  std::string p2p_dnsmasq_pid_;
};

std::unique_ptr<api::WifiDirectMedium> CreateWifiDirectMedium();

}  // namespace linux_platform
}  // namespace nearby

#endif
