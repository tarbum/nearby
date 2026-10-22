#include "internal/platform/implementation/linux/wifi_direct.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <utility>

#include "absl/strings/str_cat.h"
#include "absl/strings/string_view.h"
#include "internal/platform/exception.h"
#include "internal/platform/logging.h"

namespace nearby {
namespace linux_platform {
namespace {

constexpr char kWlanInterface[] = "wlan0";
constexpr char kHotspotProfile[] = "Hotspot";

std::string ShellQuote(absl::string_view value) {
  std::string result = "'";
  for (char c : value) {
    if (c == '\'') {
      result.append("'\\''");
    } else {
      result.push_back(c);
    }
  }
  result.push_back('\'');
  return result;
}

bool RunCommand(const std::string& command) {
  FILE* pipe = popen(command.c_str(), "r");
  if (pipe == nullptr) return false;
  char buffer[256];
  while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
  }
  int status = pclose(pipe);
  return status != -1 && WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

std::string RunCommandOutput(const std::string& command) {
  FILE* pipe = popen(command.c_str(), "r");
  if (pipe == nullptr) return "";
  std::string output;
  char buffer[256];
  while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
    output.append(buffer);
  }
  pclose(pipe);
  return output;
}

std::string RandomString(int length) {
  static constexpr char kAlphabet[] =
      "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ";
  std::random_device device;
  std::mt19937 generator(device());
  std::uniform_int_distribution<int> distribution(0,
                                                  sizeof(kAlphabet) - 2);
  std::string result;
  result.reserve(length);
  for (int i = 0; i < length; ++i) {
    result.push_back(kAlphabet[distribution(generator)]);
  }
  return result;
}

constexpr char kWpaCli[] = "/sbin/wpa_cli";
constexpr char kP2pDeviceInterface[] = "p2p-dev-wlan0";

std::string Trim(std::string value) {
  while (!value.empty() &&
         std::isspace(static_cast<unsigned char>(value.front()))) {
    value.erase(value.begin());
  }
  while (!value.empty() &&
         std::isspace(static_cast<unsigned char>(value.back()))) {
    value.pop_back();
  }
  return value;
}

void StripQuotes(std::string* value) {
  if (value->size() >= 2 && value->front() == '"' && value->back() == '"') {
    *value = value->substr(1, value->size() - 2);
  }
}

std::string WpaCli(const std::string& arguments) {
  return RunCommandOutput(absl::StrCat(kWpaCli, " ", arguments, " 2>&1"));
}

bool WpaCliOk(const std::string& arguments) {
  std::string output = WpaCli(arguments);
  return output.find("OK") != std::string::npos &&
         output.find("FAIL") == std::string::npos;
}

// Returns an empty prefix when running as root. For unprivileged processes,
// use passwordless sudo if it is available; otherwise the P2P GO path cannot
// configure its interface and the caller falls back to the NetworkManager
// hotspot.
std::string PrivilegedPrefix() {
  if (geteuid() == 0) return "";
  static const bool sudo_available =
      RunCommand("sudo -n true >/dev/null 2>&1");
  return sudo_available ? "sudo -n " : "";
}

bool CanConfigureP2pInterface() {
  return geteuid() == 0 || !PrivilegedPrefix().empty();
}

std::string LatestP2pInterface() {
  return Trim(RunCommandOutput(
      "ls -1 /sys/class/net 2>/dev/null | grep '^p2p-wlan0-' | "
      "sort -V | tail -1"));
}

std::string P2pLinkState(const std::string& interface) {
  return RunCommandOutput(
      absl::StrCat("/usr/bin/ip -o link show dev ", interface, " 2>/dev/null"));
}

bool P2pInterfaceIsUp(const std::string& interface) {
  std::string link = P2pLinkState(interface);
  return link.find(",UP") != std::string::npos ||
         link.find("<UP") != std::string::npos;
}

std::string ReadGroupNetworkValue(const std::string& interface,
                                  const std::string& key) {
  std::string value = Trim(WpaCli(absl::StrCat(
      "-i ", interface, " get_network 0 ", key)));
  StripQuotes(&value);
  return value;
}

// Waits for a P2P group interface to remain up long enough to be usable.
// Some drivers create the interface and immediately tear the AP down again;
// treat that as a failure so the caller can use the hotspot fallback.
bool WaitForP2pGroup(std::string* interface) {
  for (int attempt = 0; attempt < 20; ++attempt) {
    std::string candidate = LatestP2pInterface();
    if (!candidate.empty() && P2pInterfaceIsUp(candidate)) {
      usleep(1500000);
      if (candidate != LatestP2pInterface() ||
          !P2pInterfaceIsUp(candidate)) {
        continue;
      }
      std::string status = WpaCli(absl::StrCat("-i ", candidate, " status"));
      if (status.find("wpa_state=COMPLETED") != std::string::npos) {
        *interface = candidate;
        return true;
      }
    }
    usleep(200000);
  }
  return false;
}

}  // namespace

class WifiDirectSocketImpl::SocketInputStream final : public InputStream {
 public:
  explicit SocketInputStream(int fd) : fd_(fd) {}
  ExceptionOr<ByteArray> Read(std::int64_t size) override {
    if (size == 0) return ExceptionOr<ByteArray>{ByteArray{}};
    char buffer[16384];
    std::size_t capacity = sizeof(buffer);
    if (size > 0) capacity = std::min<std::size_t>(capacity, size);
    ssize_t received = recv(fd_, buffer, capacity, 0);
    if (received <= 0) return ExceptionOr<ByteArray>{Exception::kIo};
    return ExceptionOr<ByteArray>{
        ByteArray(std::string(buffer, static_cast<std::size_t>(received)))};
  }
  Exception Close() override { return {Exception::kSuccess}; }

 private:
  int fd_;
};

class WifiDirectSocketImpl::SocketOutputStream final : public OutputStream {
 public:
  explicit SocketOutputStream(int fd) : fd_(fd) {}
  Exception Write(absl::string_view data) override {
    std::size_t offset = 0;
    while (offset < data.size()) {
      ssize_t sent = send(fd_, data.data() + offset, data.size() - offset,
                          MSG_NOSIGNAL);
      if (sent <= 0) return {Exception::kIo};
      offset += static_cast<std::size_t>(sent);
    }
    return {Exception::kSuccess};
  }
  Exception Flush() override { return {Exception::kSuccess}; }
  Exception Close() override { return {Exception::kSuccess}; }

 private:
  int fd_;
};

WifiDirectSocketImpl::WifiDirectSocketImpl(int fd)
    : fd_(fd),
      input_(std::make_unique<SocketInputStream>(fd)),
      output_(std::make_unique<SocketOutputStream>(fd)) {}

WifiDirectSocketImpl::~WifiDirectSocketImpl() { Close(); }

Exception WifiDirectSocketImpl::Close() {
  if (!closed_) {
    closed_ = true;
    shutdown(fd_, SHUT_RDWR);
    close(fd_);
  }
  return {Exception::kSuccess};
}

WifiDirectServerSocketImpl::WifiDirectServerSocketImpl(
    WifiDirectMediumImpl* medium, int fd, int port)
    : medium_(medium), fd_(fd), port_(port) {}

WifiDirectServerSocketImpl::~WifiDirectServerSocketImpl() { Close(); }

std::string WifiDirectServerSocketImpl::GetIPAddress() const {
  return medium_ == nullptr ? "" : medium_->GetWlanIPv4();
}

std::unique_ptr<api::WifiDirectSocket> WifiDirectServerSocketImpl::Accept() {
  if (closed_) return nullptr;
  int client_fd = accept(fd_, nullptr, nullptr);
  if (client_fd < 0) return nullptr;
  int enabled = 1;
  setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &enabled, sizeof(enabled));
  return std::make_unique<WifiDirectSocketImpl>(client_fd);
}

Exception WifiDirectServerSocketImpl::Close() {
  if (!closed_) {
    closed_ = true;
    shutdown(fd_, SHUT_RDWR);
    close(fd_);
  }
  return {Exception::kSuccess};
}

void WifiDirectServerSocketImpl::PopulateWifiDirectCredentials(
    WifiDirectCredentials& wifi_direct_credentials) {
  wifi_direct_credentials.SetGateway(GetIPAddress());
  wifi_direct_credentials.SetPort(port_);
}

WifiDirectMediumImpl::~WifiDirectMediumImpl() {
  StopWifiDirect();
  DisconnectWifiDirect();
}

bool WifiDirectMediumImpl::IsInterfaceValid() const {
  return access("/sys/class/net/wlan0", F_OK) == 0;
}

std::string WifiDirectMediumImpl::GetWlanIPv4() const {
  if (mode_ == Mode::kP2p && !p2p_interface_.empty()) {
    return gateway_;
  }
  std::string output = RunCommandOutput(
      absl::StrCat("nmcli -g IP4.ADDRESS device show ", kWlanInterface));
  std::size_t end = output.find_first_of("/\n");
  std::string address = output.substr(0, end);
  if (address.empty() || address == "--") return "";
  return address;
}

std::unique_ptr<api::WifiDirectSocket> WifiDirectMediumImpl::ConnectToService(
    absl::string_view ip_address, int port,
    CancellationFlag* cancellation_flag) {
  if (cancellation_flag != nullptr && cancellation_flag->Cancelled()) {
    return nullptr;
  }
  if (ip_address.empty() || port <= 0) return nullptr;
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return nullptr;
  sockaddr_in remote{};
  remote.sin_family = AF_INET;
  remote.sin_port = htons(static_cast<uint16_t>(port));
  if (inet_pton(AF_INET, std::string(ip_address).c_str(),
                &remote.sin_addr) != 1 ||
      connect(fd, reinterpret_cast<sockaddr*>(&remote), sizeof(remote)) < 0) {
    close(fd);
    return nullptr;
  }
  int enabled = 1;
  setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &enabled, sizeof(enabled));
  return std::make_unique<WifiDirectSocketImpl>(fd);
}

std::unique_ptr<api::WifiDirectServerSocket>
WifiDirectMediumImpl::ListenForService(int port) {
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return nullptr;
  int reuse = 1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
  sockaddr_in local{};
  local.sin_family = AF_INET;
  local.sin_addr.s_addr = htonl(INADDR_ANY);
  local.sin_port = htons(static_cast<uint16_t>(port));
  if (bind(fd, reinterpret_cast<sockaddr*>(&local), sizeof(local)) < 0 ||
      listen(fd, 8) < 0) {
    close(fd);
    return nullptr;
  }
  socklen_t length = sizeof(local);
  if (getsockname(fd, reinterpret_cast<sockaddr*>(&local), &length) < 0) {
    close(fd);
    return nullptr;
  }
  return std::make_unique<WifiDirectServerSocketImpl>(
      this, fd, ntohs(local.sin_port));
}

bool WifiDirectMediumImpl::StartWifiDirect(
    WifiDirectCredentials* wifi_direct_credentials) {
  if (wifi_direct_credentials == nullptr) return false;
  StopWifiDirect();
  if (StartP2p(wifi_direct_credentials)) {
    mode_ = Mode::kP2p;
    return true;
  }
  StopP2p();
  if (StartHotspot(wifi_direct_credentials)) {
    mode_ = Mode::kHotspot;
    return true;
  }
  mode_ = Mode::kNone;
  return false;
}

bool WifiDirectMediumImpl::StartP2p(
    WifiDirectCredentials* wifi_direct_credentials) {
  if (!CanConfigureP2pInterface()) {
    LOG(INFO) << "P2P GO needs root or passwordless sudo; falling back to "
                 "NetworkManager hotspot";
    return false;
  }
  StopP2p();

  const std::string device_name = "Linux Test";
  WpaCli(absl::StrCat("-i ", kP2pDeviceInterface, " set device_name ",
                      ShellQuote(device_name)));

  if (!WpaCliOk(absl::StrCat("-i ", kP2pDeviceInterface,
                             " p2p_group_add freq=2412"))) {
    LOG(INFO) << "wpa_supplicant refused to start a P2P GO";
    return false;
  }

  std::string interface;
  if (!WaitForP2pGroup(&interface)) {
    LOG(INFO) << "P2P GO did not stay up";
    return false;
  }
  p2p_interface_ = interface;

  std::string ssid = ReadGroupNetworkValue(interface, "ssid");
  std::string password = ReadGroupNetworkValue(interface, "psk");
  if (ssid.empty() || password.empty() || password == "*") {
    LOG(INFO) << "Could not read P2P group credentials";
    return false;
  }

  const std::string prefix = PrivilegedPrefix();
  RunCommand(absl::StrCat(prefix, "/usr/bin/ip addr flush dev ", interface,
                          " >/dev/null 2>&1"));
  if (!RunCommand(absl::StrCat(prefix, "/usr/bin/ip addr add 192.168.49.1/24 "
                                          "dev ", interface)) ||
      !RunCommand(absl::StrCat(prefix, "/usr/bin/ip link set ", interface,
                               " up"))) {
    LOG(WARNING) << "Could not configure the P2P GO interface";
    return false;
  }

  p2p_dnsmasq_pid_ = absl::StrCat(
      "/tmp/quickshare-p2p-dnsmasq-", getpid(), ".pid");
  std::string dnsmasq = absl::StrCat(
      prefix, "/usr/sbin/dnsmasq --interface=", interface,
      " --bind-interfaces --dhcp-range=192.168.49.2,192.168.49.20,"
      "255.255.255.0,1h --dhcp-option=3,192.168.49.1 --port=0 "
      "--pid-file=",
      p2p_dnsmasq_pid_);
  if (!RunCommand(dnsmasq)) {
    LOG(WARNING) << "Could not start DHCP for the P2P GO";
    return false;
  }

  ssid_ = ssid;
  password_ = password;
  gateway_ = "192.168.49.1";
  wifi_direct_credentials->SetSSID(ssid_);
  wifi_direct_credentials->SetPassword(password_);
  wifi_direct_credentials->SetGateway(gateway_);
  wifi_direct_credentials->SetFrequency(2412);
  wifi_direct_credentials->SetDeviceName(device_name);
  LOG(INFO) << "Started Wi-Fi Direct P2P GO " << ssid_ << " at " << gateway_
            << " using " << interface;
  return true;
}

bool WifiDirectMediumImpl::StartHotspot(
    WifiDirectCredentials* wifi_direct_credentials) {
  ssid_ = absl::StrCat("DIRECT-qs-", RandomString(4));
  password_ = RandomString(12);
  RunCommand(absl::StrCat("nmcli connection delete ", kHotspotProfile,
                          " >/dev/null 2>&1"));
  std::string command = absl::StrCat(
      "nmcli device wifi hotspot ifname ", kWlanInterface, " ssid ",
      ShellQuote(ssid_), " password ", ShellQuote(password_));
  if (!RunCommand(command)) {
    LOG(WARNING) << "Failed to create NetworkManager Wi-Fi hotspot";
    return false;
  }
  for (int attempt = 0; attempt < 20; ++attempt) {
    gateway_ = GetWlanIPv4();
    if (!gateway_.empty()) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
  }
  if (gateway_.empty()) return false;
  wifi_direct_credentials->SetSSID(ssid_);
  wifi_direct_credentials->SetPassword(password_);
  wifi_direct_credentials->SetGateway(gateway_);
  wifi_direct_credentials->SetFrequency(2412);
  wifi_direct_credentials->SetDeviceName("Linux Test");
  LOG(INFO) << "Started Wi-Fi hotspot " << ssid_ << " at " << gateway_;
  return true;
}

void WifiDirectMediumImpl::StopP2p() {
  const std::string prefix = PrivilegedPrefix();
  if (!p2p_dnsmasq_pid_.empty()) {
    RunCommand(absl::StrCat(prefix, "kill $(cat ",
                            ShellQuote(p2p_dnsmasq_pid_),
                            ") >/dev/null 2>&1"));
    RunCommand(absl::StrCat(prefix, "rm -f ",
                            ShellQuote(p2p_dnsmasq_pid_)));
    p2p_dnsmasq_pid_.clear();
  }
  if (!p2p_interface_.empty()) {
    WpaCli(absl::StrCat("-i ", kP2pDeviceInterface, " p2p_group_remove ",
                        ShellQuote(p2p_interface_)));
    p2p_interface_.clear();
  }
  ssid_.clear();
  password_.clear();
  gateway_.clear();
}

bool WifiDirectMediumImpl::StopWifiDirect() {
  StopP2p();
  bool result = RunCommand(absl::StrCat("nmcli connection down ",
                                        kHotspotProfile, " >/dev/null 2>&1"));
  RunCommand(absl::StrCat("nmcli connection delete ", kHotspotProfile,
                          " >/dev/null 2>&1"));
  mode_ = Mode::kNone;
  return result;
}

bool WifiDirectMediumImpl::ConnectWifiDirect(
    const WifiDirectCredentials& wifi_direct_credentials) {
  std::string ssid = wifi_direct_credentials.GetSSID();
  std::string password = wifi_direct_credentials.GetPassword();
  if (ssid.empty()) return false;
  joined_ssid_ = ssid;
  std::string connections = RunCommandOutput(
      "nmcli -g NAME,TYPE connection show");
  std::size_t line_start = 0;
  while (line_start < connections.size()) {
    std::size_t line_end = connections.find('\n', line_start);
    if (line_end == std::string::npos) line_end = connections.size();
    std::string line = connections.substr(line_start, line_end - line_start);
    if (line.find(":wifi") != std::string::npos) {
      std::string name = line.substr(0, line.find(':'));
      if (name.find("qs-wfd-") != 0 && name != kHotspotProfile) {
        RunCommand(absl::StrCat("nmcli connection modify id ",
                                ShellQuote(name),
                                " connection.autoconnect no >/dev/null 2>&1"));
        disabled_profiles_.push_back(name);
      }
    }
    line_start = line_end + 1;
  }
  std::string active = RunCommandOutput(
      "nmcli -g NAME,TYPE,DEVICE connection show --active");
  std::size_t active_start = 0;
  while (active_start < active.size()) {
    std::size_t active_end = active.find('\n', active_start);
    if (active_end == std::string::npos) active_end = active.size();
    std::string line = active.substr(active_start, active_end - active_start);
    if (line.find(":wifi:wlan0") != std::string::npos) {
      std::string name = line.substr(0, line.find(':'));
      previous_wifi_ = name;
      RunCommand(absl::StrCat("nmcli connection down id ", ShellQuote(name),
                              " >/dev/null 2>&1"));
      break;
    }
    active_start = active_end + 1;
  }
  std::string profile = absl::StrCat("qs-wfd-", RandomString(6));
  joined_profile_ = profile;
  RunCommand(absl::StrCat("nmcli connection delete id ", ShellQuote(profile),
                          " >/dev/null 2>&1"));
  std::string add = absl::StrCat(
      "nmcli connection add type wifi ifname ", kWlanInterface, " con-name ",
      ShellQuote(profile), " ssid ", ShellQuote(ssid),
      " 802-11-wireless.hidden yes wifi-sec.key-mgmt wpa-psk wifi-sec.psk ",
      ShellQuote(password));
  if (!RunCommand(add) ||
      !RunCommand(absl::StrCat("nmcli connection up id ",
                               ShellQuote(profile)))) {
    LOG(WARNING) << "Failed to join Wi-Fi Direct network " << ssid;
    return false;
  }
  for (int attempt = 0; attempt < 20; ++attempt) {
    std::string address = GetWlanIPv4();
    if (!address.empty() && address.rfind("192.168.49.", 0) == 0) {
      LOG(INFO) << "Joined Wi-Fi Direct network " << ssid << " at "
                << address;
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
  }
  LOG(WARNING) << "Wi-Fi Direct network " << ssid
               << " did not obtain a 192.168.49.x address";
  return false;
}

bool WifiDirectMediumImpl::DisconnectWifiDirect() {
  if (joined_ssid_.empty()) return true;
  bool result = RunCommand(absl::StrCat("nmcli connection down id ",
                                        ShellQuote(joined_profile_), 
                                        " >/dev/null 2>&1"));
  RunCommand(absl::StrCat("nmcli connection delete id ",
                          ShellQuote(joined_profile_), " >/dev/null 2>&1"));
  joined_profile_.clear();
  joined_ssid_.clear();
  for (const auto& profile_name : disabled_profiles_) {
    RunCommand(absl::StrCat("nmcli connection modify id ",
                            ShellQuote(profile_name),
                            " connection.autoconnect yes >/dev/null 2>&1"));
  }
  disabled_profiles_.clear();
  if (!previous_wifi_.empty()) {
    RunCommand(absl::StrCat("nmcli connection up id ",
                            ShellQuote(previous_wifi_), " >/dev/null 2>&1"));
    previous_wifi_.clear();
  }
  return result;
}

std::unique_ptr<api::WifiDirectMedium> CreateWifiDirectMedium() {
  return std::make_unique<WifiDirectMediumImpl>();
}

}  // namespace linux_platform
}  // namespace nearby
