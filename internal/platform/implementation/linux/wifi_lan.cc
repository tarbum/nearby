#include "internal/platform/implementation/linux/wifi_lan.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netdb.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <map>
#include <mutex>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "absl/strings/str_cat.h"
#include "absl/strings/string_view.h"
#include "internal/platform/byte_array.h"
#include "internal/platform/cancellation_flag.h"
#include "internal/platform/exception.h"
#include "internal/platform/implementation/upgrade_address_info.h"
#include "internal/platform/logging.h"
#include "internal/platform/nsd_service_info.h"
#include "internal/platform/service_address.h"

namespace nearby {
namespace linux_platform {
namespace {

std::string LocalIPv4() {
  struct ifaddrs* interfaces = nullptr;
  if (getifaddrs(&interfaces) != 0) return "";
  std::string result;
  for (struct ifaddrs* ifa = interfaces; ifa != nullptr; ifa = ifa->ifa_next) {
    if (ifa->ifa_addr == nullptr || ifa->ifa_addr->sa_family != AF_INET) {
      continue;
    }
    if ((ifa->ifa_flags & IFF_LOOPBACK) != 0) continue;
    char address[INET_ADDRSTRLEN] = {};
    auto* sin = reinterpret_cast<struct sockaddr_in*>(ifa->ifa_addr);
    if (inet_ntop(AF_INET, &sin->sin_addr, address, sizeof(address)) != nullptr) {
      result = address;
      break;
    }
  }
  freeifaddrs(interfaces);
  return result;
}

std::string NormalizeServiceType(std::string service_type) {
  while (!service_type.empty() && service_type.back() == '.') {
    service_type.pop_back();
  }
  return service_type;
}

pid_t Spawn(const std::vector<std::string>& args, int* read_fd, bool capture) {
  int pipe_fds[2] = {-1, -1};
  if (capture && pipe(pipe_fds) != 0) return -1;

  std::vector<char*> argv;
  argv.reserve(args.size() + 1);
  for (const auto& arg : args) {
    argv.push_back(const_cast<char*>(arg.c_str()));
  }
  argv.push_back(nullptr);

  pid_t pid = fork();
  if (pid < 0) {
    if (capture) {
      close(pipe_fds[0]);
      close(pipe_fds[1]);
    }
    return -1;
  }

  if (pid == 0) {
    if (capture) {
      dup2(pipe_fds[1], STDOUT_FILENO);
      close(pipe_fds[0]);
      close(pipe_fds[1]);
    } else {
      int devnull = open("/dev/null", O_WRONLY);
      if (devnull >= 0) {
        dup2(devnull, STDOUT_FILENO);
        dup2(devnull, STDERR_FILENO);
        close(devnull);
      }
    }
    execvp(argv[0], argv.data());
    _exit(127);
  }

  if (capture) {
    close(pipe_fds[1]);
    *read_fd = pipe_fds[0];
  }
  return pid;
}

void StopProcess(pid_t pid) {
  if (pid <= 0) return;
  kill(pid, SIGTERM);
  waitpid(pid, nullptr, 0);
}

class SocketInputStream final : public InputStream {
 public:
  explicit SocketInputStream(int* fd) : fd_(fd) {}

  ExceptionOr<ByteArray> Read(std::int64_t size) override {
    if (*fd_ < 0 || size <= 0) return ExceptionOr<ByteArray>{ByteArray{}};
    std::string data(static_cast<size_t>(size), '\0');
    ssize_t received;
    do {
      received = recv(*fd_, data.data(), data.size(), 0);
    } while (received < 0 && errno == EINTR);
    if (received < 0) return ExceptionOr<ByteArray>{Exception::kIo};
    if (received == 0) return ExceptionOr<ByteArray>{ByteArray{}};
    data.resize(static_cast<size_t>(received));
    return ExceptionOr<ByteArray>{ByteArray(std::move(data))};
  }

  Exception Close() override { return {Exception::kSuccess}; }

 private:
  int* fd_;
};

class SocketOutputStream final : public OutputStream {
 public:
  explicit SocketOutputStream(int* fd) : fd_(fd) {}

  Exception Write(absl::string_view data) override {
    if (*fd_ < 0) return {Exception::kIo};
    size_t offset = 0;
    while (offset < data.size()) {
      ssize_t sent;
      do {
        sent = send(*fd_, data.data() + offset, data.size() - offset,
                    MSG_NOSIGNAL);
      } while (sent < 0 && errno == EINTR);
      if (sent <= 0) return {Exception::kIo};
      offset += static_cast<size_t>(sent);
    }
    return {Exception::kSuccess};
  }

  Exception Flush() override { return {Exception::kSuccess}; }
  Exception Close() override { return {Exception::kSuccess}; }

 private:
  int* fd_;
};

class WifiLanSocketImpl final : public api::WifiLanSocket {
 public:
  explicit WifiLanSocketImpl(int fd)
      : fd_(fd), input_stream_(&fd_), output_stream_(&fd_) {}
  ~WifiLanSocketImpl() override { Close(); }

  InputStream& GetInputStream() override { return input_stream_; }
  OutputStream& GetOutputStream() override { return output_stream_; }

  Exception Close() override {
    if (fd_ >= 0) {
      shutdown(fd_, SHUT_RDWR);
      close(fd_);
      fd_ = -1;
    }
    return {Exception::kSuccess};
  }

 private:
  int fd_;
  SocketInputStream input_stream_;
  SocketOutputStream output_stream_;
};

class WifiLanServerSocketImpl final : public api::WifiLanServerSocket {
 public:
  WifiLanServerSocketImpl(int fd, int port, std::string ip_address)
      : fd_(fd), port_(port), ip_address_(std::move(ip_address)) {}
  ~WifiLanServerSocketImpl() override { Close(); }

  std::string GetIPAddress() const override { return ip_address_; }
  int GetPort() const override { return port_; }

  std::unique_ptr<api::WifiLanSocket> Accept() override {
    if (fd_ < 0) return nullptr;
    int client;
    do {
      client = accept(fd_, nullptr, nullptr);
    } while (client < 0 && errno == EINTR);
    if (client < 0) return nullptr;
    return std::make_unique<WifiLanSocketImpl>(client);
  }

  Exception Close() override {
    if (fd_ >= 0) {
      shutdown(fd_, SHUT_RDWR);
      close(fd_);
      fd_ = -1;
    }
    return {Exception::kSuccess};
  }

 private:
  int fd_;
  int port_;
  std::string ip_address_;
};

class DiscoveryProcess {
 public:
  DiscoveryProcess(std::string service_type,
                   api::WifiLanMedium::DiscoveredServiceCallback callback)
      : service_type_(std::move(service_type)), callback_(std::move(callback)) {}
  ~DiscoveryProcess() { Stop(); }

  bool Start() {
    int fd = -1;
    pid_ = Spawn(
        {"avahi-browse", "-pr", NormalizeServiceType(service_type_)}, &fd, true);
    if (pid_ <= 0) return false;
    fd_ = fd;
    thread_ = std::thread([this]() { Run(); });
    return true;
  }

  void Stop() {
    stopped_ = true;
    if (fd_ >= 0) {
      close(fd_);
      fd_ = -1;
    }
    StopProcess(pid_);
    pid_ = -1;
    if (thread_.joinable()) thread_.join();
  }

 private:
  void Run() {
    std::string pending;
    char buffer[4096];
    while (!stopped_) {
      ssize_t read_size = read(fd_, buffer, sizeof(buffer));
      if (read_size < 0 && errno == EINTR) continue;
      if (read_size <= 0) break;
      pending.append(buffer, static_cast<size_t>(read_size));
      size_t newline;
      while ((newline = pending.find('\n')) != std::string::npos) {
        std::string line = pending.substr(0, newline);
        pending.erase(0, newline + 1);
        HandleLine(line);
      }
    }
  }

  static void SetTxtRecords(const std::string& txt, NsdServiceInfo& info) {
    size_t position = 0;
    while (true) {
      size_t start = txt.find('"', position);
      if (start == std::string::npos) break;
      size_t end = txt.find('"', start + 1);
      if (end == std::string::npos) break;
      std::string token = txt.substr(start + 1, end - start - 1);
      size_t equals = token.find('=');
      if (equals != std::string::npos) {
        info.SetTxtRecord(token.substr(0, equals), token.substr(equals + 1));
      }
      position = end + 1;
    }
  }

  void HandleLine(const std::string& line) {
    if (line.size() < 2 || (line[0] != '=' && line[0] != '-')) return;
    std::vector<std::string> fields;
    std::stringstream stream(line);
    std::string field;
    while (std::getline(stream, field, ';')) fields.push_back(field);
    if (fields.size() < 6) return;
    const std::string& name = fields[3];
    if (line[0] == '-') {
      auto it = discovered_.find(name);
      if (it != discovered_.end()) {
        callback_.service_lost_cb(it->second);
        discovered_.erase(it);
      }
      return;
    }
    if (fields.size() < 10) return;
    NsdServiceInfo info;
    info.SetServiceName(name);
    info.SetServiceType(service_type_);
    info.SetIPAddress(fields[7]);
    info.SetPort(std::stoi(fields[8]));
    SetTxtRecords(fields[9], info);
    discovered_[name] = info;
    callback_.service_discovered_cb(info);
  }

  std::string service_type_;
  api::WifiLanMedium::DiscoveredServiceCallback callback_;
  std::map<std::string, NsdServiceInfo> discovered_;
  pid_t pid_ = -1;
  int fd_ = -1;
  std::atomic_bool stopped_{false};
  std::thread thread_;
};

class WifiLanMediumImpl final : public api::WifiLanMedium {
 public:
  bool IsNetworkConnected() const override { return !LocalIPv4().empty(); }

  bool StartAdvertising(const NsdServiceInfo& info) override {
    std::lock_guard<std::mutex> lock(mutex_);
    std::string service_type = NormalizeServiceType(info.GetServiceType());
    if (advertising_.contains(service_type)) return false;

    std::vector<std::string> args{"avahi-publish-service", "-s",
                                  info.GetServiceName(), service_type,
                                  std::to_string(info.GetPort())};
    for (const auto& [key, value] : info.GetTxtRecords()) {
      args.push_back(absl::StrCat(key, "=", value));
    }
    pid_t pid = Spawn(args, nullptr, false);
    if (pid <= 0) return false;
    advertising_[service_type] = pid;
    return true;
  }

  bool StopAdvertising(const NsdServiceInfo& info) override {
    std::lock_guard<std::mutex> lock(mutex_);
    std::string service_type = NormalizeServiceType(info.GetServiceType());
    auto it = advertising_.find(service_type);
    if (it == advertising_.end()) return false;
    StopProcess(it->second);
    advertising_.erase(it);
    return true;
  }

  bool StartDiscovery(const std::string& service_type,
                      DiscoveredServiceCallback callback) override {
    std::lock_guard<std::mutex> lock(mutex_);
    std::string type = NormalizeServiceType(service_type);
    if (discovery_.contains(type)) return false;
    auto process =
        std::make_unique<DiscoveryProcess>(service_type, std::move(callback));
    if (!process->Start()) return false;
    discovery_[type] = std::move(process);
    return true;
  }

  bool StopDiscovery(const std::string& service_type) override {
    std::lock_guard<std::mutex> lock(mutex_);
    std::string type = NormalizeServiceType(service_type);
    auto it = discovery_.find(type);
    if (it == discovery_.end()) return false;
    it->second->Stop();
    discovery_.erase(it);
    return true;
  }

  std::unique_ptr<api::WifiLanSocket> ConnectToService(
      const NsdServiceInfo& info,
      CancellationFlag* cancellation_flag) override {
    return Connect(info.GetIPAddress(), info.GetPort(), cancellation_flag);
  }

  std::unique_ptr<api::WifiLanSocket> ConnectToService(
      const ServiceAddress& address,
      CancellationFlag* cancellation_flag) override {
    char text[INET6_ADDRSTRLEN] = {};
    int family = address.address.size() == 4 ? AF_INET : AF_INET6;
    if (inet_ntop(family, address.address.data(), text, sizeof(text)) == nullptr) {
      return nullptr;
    }
    return Connect(text, address.port, cancellation_flag);
  }

  std::unique_ptr<api::WifiLanServerSocket> ListenForService(
      int port = 0) override {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return nullptr;
    int reuse = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(static_cast<uint16_t>(port));
    if (bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 ||
        listen(fd, 16) != 0) {
      close(fd);
      return nullptr;
    }

    socklen_t length = sizeof(address);
    if (getsockname(fd, reinterpret_cast<sockaddr*>(&address), &length) != 0) {
      close(fd);
      return nullptr;
    }
    return std::make_unique<WifiLanServerSocketImpl>(
        fd, ntohs(address.sin_port), LocalIPv4());
  }

  absl::optional<std::pair<std::int32_t, std::int32_t>> GetDynamicPortRange()
      override {
    return std::make_pair(49152, 65535);
  }

  api::UpgradeAddressInfo GetUpgradeAddressCandidates(
      const api::WifiLanServerSocket& server_socket) override {
    api::UpgradeAddressInfo result;
    std::string ip = server_socket.GetIPAddress();
    in_addr address{};
    if (inet_pton(AF_INET, ip.c_str(), &address) != 1) return result;
    ServiceAddress candidate;
    auto* bytes = reinterpret_cast<const char*>(&address);
    candidate.address.assign(bytes, bytes + sizeof(address));
    candidate.port = static_cast<uint16_t>(server_socket.GetPort());
    result.address_candidates.push_back(candidate);
    result.num_interfaces = 1;
    return result;
  }

 private:
  std::unique_ptr<api::WifiLanSocket> Connect(
      const std::string& ip_address, int port,
      CancellationFlag* cancellation_flag) {
    if (cancellation_flag != nullptr && cancellation_flag->Cancelled()) {
      return nullptr;
    }
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* addresses = nullptr;
    if (getaddrinfo(ip_address.c_str(), std::to_string(port).c_str(), &hints,
                    &addresses) != 0) {
      return nullptr;
    }
    int fd = -1;
    for (addrinfo* address = addresses; address != nullptr;
         address = address->ai_next) {
      fd = socket(address->ai_family, address->ai_socktype, address->ai_protocol);
      if (fd < 0) continue;
      if (connect(fd, address->ai_addr, address->ai_addrlen) == 0) break;
      close(fd);
      fd = -1;
    }
    freeaddrinfo(addresses);
    if (fd < 0) return nullptr;
    return std::make_unique<WifiLanSocketImpl>(fd);
  }

  std::mutex mutex_;
  std::map<std::string, pid_t> advertising_;
  std::map<std::string, std::unique_ptr<DiscoveryProcess>> discovery_;
};

}  // namespace

std::unique_ptr<api::WifiLanMedium> CreateWifiLanMedium() {
  return std::make_unique<WifiLanMediumImpl>();
}

}  // namespace linux_platform
}  // namespace nearby
