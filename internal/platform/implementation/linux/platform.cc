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

#include "internal/platform/implementation/platform.h"

#include <openssl/sha.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <iostream>
#include <map>
#include <sstream>
#include <memory>
#include <mutex>
#include <optional>
#include <queue>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "absl/functional/any_invocable.h"
#include "absl/status/status.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/string_view.h"
#include "absl/time/clock.h"
#include "absl/time/time.h"
#include "internal/base/file_path.h"
#include "internal/platform/byte_array.h"
#include "internal/platform/exception.h"
#include "internal/platform/implementation/atomic_boolean.h"
#include "internal/platform/implementation/atomic_reference.h"
#include "internal/platform/implementation/ble.h"
#include "internal/platform/implementation/bluetooth_adapter.h"
#include "internal/platform/implementation/bluetooth_classic.h"
#include "internal/platform/implementation/condition_variable.h"
#include "internal/platform/implementation/count_down_latch.h"
#include "internal/platform/implementation/crypto.h"
#include "internal/platform/implementation/input_file.h"
#include "internal/platform/implementation/log_message.h"
#include "internal/platform/implementation/mutex.h"
#include "internal/platform/implementation/output_file.h"
#include "internal/platform/implementation/scheduled_executor.h"
#include "internal/platform/implementation/submittable_executor.h"
#include "internal/platform/implementation/timer.h"
#include "internal/platform/implementation/wifi.h"
#include "internal/platform/implementation/wifi_aware.h"
#include "internal/platform/implementation/wifi_direct.h"
#include "internal/platform/implementation/wifi_hotspot.h"
#include "internal/platform/implementation/wifi_lan.h"
#include "internal/platform/implementation/shared/count_down_latch.h"
#include "internal/platform/implementation/linux/wifi_lan.h"
#include "internal/platform/implementation/shared/file.h"
#include "internal/platform/implementation/system_clock.h"
#include "internal/platform/logging.h"
#include "internal/platform/nsd_service_info.h"
#include "internal/platform/service_address.h"

namespace nearby {
namespace linux_platform {
namespace {

using ::nearby::api::LogMessage;

constexpr char kDefaultQuickshareDirectory[] = "quickshare";

std::string HomeDirectory() {
  const char* home = std::getenv("HOME");
  return home == nullptr ? std::string("/tmp") : std::string(home);
}

std::string DataDirectory() {
  const char* data = std::getenv("XDG_DATA_HOME");
  if (data != nullptr && data[0] != '\0') {
    return std::string(data);
  }
  return absl::StrCat(HomeDirectory(), "/.local/share");
}

class LogMessageImpl final : public LogMessage {
 public:
  LogMessageImpl(const char* file, int line, Severity severity)
      : file_(file), line_(line), severity_(severity) {}
  ~LogMessageImpl() override {
    if (stream_.str().empty()) return;
    std::fprintf(stderr, "%s:%d %s", file_, line_, stream_.str().c_str());
    if (severity_ == Severity::kFatal) std::abort();
  }

  void Print(const char* format, ...) override {
    va_list args;
    va_start(args, format);
    std::vfprintf(stderr, format, args);
    va_end(args);
  }

  std::ostream& Stream() override { return stream_; }

 private:
  const char* file_;
  int line_;
  Severity severity_;
  std::ostringstream stream_;
};

class AtomicBooleanImpl final : public api::AtomicBoolean {
 public:
  explicit AtomicBooleanImpl(bool value) : value_(value) {}
  bool Get() const override { return value_.load(); }
  bool Set(bool value) override { return value_.exchange(value); }

 private:
  std::atomic_bool value_;
};

class AtomicUint32Impl final : public api::AtomicUint32 {
 public:
  explicit AtomicUint32Impl(std::uint32_t value) : value_(value) {}
  std::uint32_t Get() const override { return value_.load(); }
  void Set(std::uint32_t value) override { value_.store(value); }

 private:
  std::atomic_uint32_t value_;
};

class MutexImpl final : public api::Mutex {
 public:
  explicit MutexImpl(Mode mode) : mode_(mode) {}
  ~MutexImpl() override = default;

  void Lock() override {
    if (mode_ == Mode::kRecursive) {
      recursive_mutex_.lock();
    } else {
      mutex_.lock();
    }
  }

  void Unlock() override {
    if (mode_ == Mode::kRecursive) {
      recursive_mutex_.unlock();
    } else {
      mutex_.unlock();
    }
  }

  void AssertHeld() const override {}

  std::mutex& GetMutex() { return mutex_; }

 private:
  friend class ConditionVariableImpl;
  Mode mode_;
  std::mutex mutex_;
  std::recursive_mutex recursive_mutex_;
};

class ConditionVariableImpl final : public api::ConditionVariable {
 public:
  explicit ConditionVariableImpl(MutexImpl* mutex) : mutex_(mutex) {}

  void Notify() override { condition_.notify_all(); }

  Exception Wait() override {
    std::unique_lock<std::mutex> lock(mutex_->GetMutex(), std::adopt_lock);
    condition_.wait(lock);
    lock.release();
    return {Exception::kSuccess};
  }

  Exception Wait(absl::Duration timeout) override {
    std::unique_lock<std::mutex> lock(mutex_->GetMutex(), std::adopt_lock);
    condition_.wait_for(
        lock, std::chrono::nanoseconds(absl::ToInt64Nanoseconds(timeout)));
    lock.release();
    return {Exception::kSuccess};
  }

 private:
  MutexImpl* mutex_;
  std::condition_variable condition_;
};

class ThreadPool {
 public:
  explicit ThreadPool(int workers) {
    for (int i = 0; i < std::max(1, workers); ++i) {
      workers_.emplace_back([this]() { Worker(); });
    }
  }

  ~ThreadPool() { Shutdown(); }

  bool Submit(Runnable&& runnable) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (shutdown_) return false;
    tasks_.push(std::move(runnable));
    condition_.notify_one();
    return true;
  }

  void Shutdown() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (shutdown_) return;
      shutdown_ = true;
    }
    condition_.notify_all();
    for (auto& worker : workers_) {
      if (worker.joinable()) worker.join();
    }
  }

  bool IsShutdown() const { return shutdown_; }

 private:
  void Worker() {
    while (true) {
      Runnable task;
      {
        std::unique_lock<std::mutex> lock(mutex_);
        condition_.wait(lock, [this]() { return shutdown_ || !tasks_.empty(); });
        if (shutdown_ && tasks_.empty()) return;
        task = std::move(tasks_.front());
        tasks_.pop();
      }
      task();
    }
  }

  mutable std::mutex mutex_;
  std::condition_variable condition_;
  std::queue<Runnable> tasks_;
  std::vector<std::thread> workers_;
  bool shutdown_ = false;
};

class MultiThreadExecutorImpl final : public api::SubmittableExecutor {
 public:
  explicit MultiThreadExecutorImpl(int max_parallelism)
      : pool_(max_parallelism) {}

  void Execute(Runnable&& runnable) override { pool_.Submit(std::move(runnable)); }
  bool DoSubmit(Runnable&& runnable) override {
    return pool_.Submit(std::move(runnable));
  }
  void Shutdown() override { pool_.Shutdown(); }

 private:
  ThreadPool pool_;
};

class SingleThreadExecutorImpl final : public api::SubmittableExecutor {
 public:
  SingleThreadExecutorImpl() : pool_(1) {}
  void Execute(Runnable&& runnable) override { pool_.Submit(std::move(runnable)); }
  bool DoSubmit(Runnable&& runnable) override {
    return pool_.Submit(std::move(runnable));
  }
  void Shutdown() override { pool_.Shutdown(); }

 private:
  ThreadPool pool_;
};

class ScheduledTask final : public api::Cancelable {
 public:
  bool Cancel() override {
    bool expected = false;
    return canceled_.compare_exchange_strong(expected, true);
  }

  bool IsCanceled() const { return canceled_.load(); }

  void Run() {
    if (!IsCanceled() && runnable_) runnable_();
  }

  void SetRunnable(Runnable runnable) { runnable_ = std::move(runnable); }

 private:
  std::atomic_bool canceled_{false};
  Runnable runnable_;
};

class ScheduledExecutorImpl final : public api::ScheduledExecutor {
 public:
  ScheduledExecutorImpl() : worker_([this]() { Run(); }) {}
  ~ScheduledExecutorImpl() override { Shutdown(); }

  void Execute(Runnable&& runnable) override {
    Schedule(std::move(runnable), absl::ZeroDuration());
  }

  std::shared_ptr<api::Cancelable> Schedule(Runnable&& runnable,
                                             absl::Duration delay) override {
    auto task = std::make_shared<ScheduledTask>();
    task->SetRunnable(std::move(runnable));
    auto trigger =
        std::chrono::steady_clock::now() +
        std::chrono::nanoseconds(absl::ToInt64Nanoseconds(delay));
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (shutdown_) return task;
      tasks_.emplace(trigger, task);
    }
    condition_.notify_one();
    return task;
  }

  void Shutdown() override {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (shutdown_) return;
      shutdown_ = true;
    }
    condition_.notify_all();
    if (worker_.joinable()) worker_.join();
  }

 private:
  void Run() {
    std::unique_lock<std::mutex> lock(mutex_);
    while (!shutdown_) {
      if (tasks_.empty()) {
        condition_.wait(lock);
        continue;
      }
      auto next = tasks_.begin();
      if (next->first > std::chrono::steady_clock::now()) {
        condition_.wait_until(lock, next->first);
        continue;
      }
      auto task = next->second;
      tasks_.erase(next);
      lock.unlock();
      task->Run();
      lock.lock();
    }
  }

  std::mutex mutex_;
  std::condition_variable condition_;
  std::multimap<std::chrono::steady_clock::time_point,
                std::shared_ptr<ScheduledTask>>
      tasks_;
  bool shutdown_ = false;
  std::thread worker_;
};

class TimerImpl final : public api::Timer {
 public:
  explicit TimerImpl(api::ScheduledExecutor* executor) : executor_(executor) {}
  ~TimerImpl() override { Stop(); }

  bool Create(int delay, int interval,
              absl::AnyInvocable<void()> callback) override {
    if (delay < 0 || interval < 0 || callback == nullptr) return false;
    callback_ = std::move(callback);
    interval_ = absl::Milliseconds(interval);
    stopped_ = false;
    return Schedule(absl::Milliseconds(delay));
  }

  bool Stop() override {
    stopped_ = true;
    std::lock_guard<std::mutex> lock(mutex_);
    if (task_) {
      bool canceled = task_->Cancel();
      task_.reset();
      return canceled;
    }
    return true;
  }

 private:
  bool Schedule(absl::Duration delay) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopped_) return false;
    task_ = executor_->Schedule(
        [this]() {
          if (stopped_) return;
          if (interval_ != absl::ZeroDuration()) Schedule(interval_);
          callback_();
        },
        delay);
    return true;
  }

  api::ScheduledExecutor* executor_;
  std::mutex mutex_;
  std::shared_ptr<api::Cancelable> task_;
  absl::AnyInvocable<void()> callback_;
  absl::Duration interval_;
  std::atomic_bool stopped_{true};
};

class DeviceInfoImpl final : public api::DeviceInfo {
 public:
  std::optional<std::string> GetOsDeviceName() const override {
    char name[256] = {};
    if (gethostname(name, sizeof(name)) != 0) return std::nullopt;
    return std::string(name);
  }

  DeviceType GetDeviceType() const override { return DeviceType::kLaptop; }
  OsType GetOsType() const override { return OsType::kUnknown; }

  FilePath GetDownloadPath() const override {
    return FilePath(absl::StrCat(HomeDirectory(), "/Downloads"));
  }

  FilePath GetLocalAppDataPath(FilePath sub_path) const override {
    FilePath path(absl::StrCat(DataDirectory(), "/", kDefaultQuickshareDirectory));
    path.append(sub_path);
    return path;
  }

  FilePath GetTemporaryPath() const override { return FilePath("/tmp"); }
  FilePath GetLogPath() const override { return FilePath("/tmp"); }

  bool IsScreenLocked() const override { return false; }

  void RegisterScreenLockedListener(
      absl::string_view listener_name,
      std::function<void(ScreenStatus)> callback) override {}

  void UnregisterScreenLockedListener(absl::string_view listener_name) override {}

  bool PreventSleep() override { return true; }
  bool AllowSleep() override { return true; }
};

class WifiMediumImpl final : public api::WifiMedium {
 public:
  WifiMediumImpl() {
    information_.is_connected = true;
    information_.ap_frequency = -1;
  }

  bool IsInterfaceValid() const override { return true; }
  api::WifiCapability& GetCapability() override { return capability_; }
  api::WifiInformation& GetInformation() override { return information_; }

 private:
  api::WifiCapability capability_;
  api::WifiInformation information_;
};

class BluetoothAdapterImpl final : public api::BluetoothAdapter {
 public:
  bool SetStatus(Status status) override { return false; }
  bool IsEnabled() const override { return false; }
  ScanMode GetScanMode() const override { return ScanMode::kNone; }
  bool SetScanMode(ScanMode scan_mode) override { return false; }
  std::string GetName() const override { return ""; }
  bool SetName(absl::string_view name, bool persist) override { return false; }
  MacAddress GetMacAddress() const override { return MacAddress(); }
};

class AppLifecycleMonitorImpl final : public api::AppLifecycleMonitor {
 public:
  explicit AppLifecycleMonitorImpl(
      std::function<void(AppLifecycleState)> callback)
      : api::AppLifecycleMonitor(std::move(callback)) {}
};

}  // namespace

}  // namespace linux_platform

namespace api {

int GetCurrentTid() {
  return static_cast<int>(syscall(SYS_gettid));
}

std::string ImplementationPlatform::GetCustomSavePath(
    const std::string& save_path, const std::string& parent_folder,
    const std::string& file_name) {
  return absl::StrCat(save_path, "/", parent_folder, "/", file_name);
}

OSName ImplementationPlatform::GetCurrentOS() { return OSName::kLinux; }

std::unique_ptr<AtomicBoolean> ImplementationPlatform::CreateAtomicBoolean(
    bool initial_value) {
  return std::make_unique<linux_platform::AtomicBooleanImpl>(initial_value);
}

std::unique_ptr<AtomicUint32> ImplementationPlatform::CreateAtomicUint32(
    std::uint32_t value) {
  return std::make_unique<linux_platform::AtomicUint32Impl>(value);
}

std::unique_ptr<CountDownLatch> ImplementationPlatform::CreateCountDownLatch(
    std::int32_t count) {
  return std::make_unique<shared::CountDownLatch>(count);
}

std::unique_ptr<Mutex> ImplementationPlatform::CreateMutex(Mutex::Mode mode) {
  return std::make_unique<linux_platform::MutexImpl>(mode);
}

std::unique_ptr<ConditionVariable>
ImplementationPlatform::CreateConditionVariable(Mutex* mutex) {
  return std::make_unique<linux_platform::ConditionVariableImpl>(
      static_cast<linux_platform::MutexImpl*>(mutex));
}

std::unique_ptr<InputFile> ImplementationPlatform::CreateInputFile(
    const std::string& file_path) {
  return shared::IOFile::CreateInputFile(file_path);
}

std::unique_ptr<OutputFile> ImplementationPlatform::CreateOutputFile(
    const std::string& file_path) {
  std::error_code error;
  std::filesystem::create_directories(
      std::filesystem::path(file_path).parent_path(), error);
  return shared::IOFile::CreateOutputFile(file_path);
}

std::unique_ptr<LogMessage> ImplementationPlatform::CreateLogMessage(
    const char* file, int line, LogMessage::Severity severity) {
  return std::make_unique<linux_platform::LogMessageImpl>(file, line, severity);
}

std::unique_ptr<SubmittableExecutor>
ImplementationPlatform::CreateSingleThreadExecutor() {
  return std::make_unique<linux_platform::SingleThreadExecutorImpl>();
}

std::unique_ptr<SubmittableExecutor>
ImplementationPlatform::CreateMultiThreadExecutor(int max_concurrency) {
  return std::make_unique<linux_platform::MultiThreadExecutorImpl>(
      max_concurrency);
}

std::unique_ptr<ScheduledExecutor>
ImplementationPlatform::CreateScheduledExecutor() {
  return std::make_unique<linux_platform::ScheduledExecutorImpl>();
}

std::unique_ptr<AwdlMedium> ImplementationPlatform::CreateAwdlMedium() {
  return nullptr;
}

std::unique_ptr<BluetoothAdapter>
ImplementationPlatform::CreateBluetoothAdapter() {
  return std::make_unique<linux_platform::BluetoothAdapterImpl>();
}

std::unique_ptr<BluetoothClassicMedium>
ImplementationPlatform::CreateBluetoothClassicMedium(BluetoothAdapter&) {
  return nullptr;
}

std::unique_ptr<api::ble::BleMedium> ImplementationPlatform::CreateBleMedium(
    api::BluetoothAdapter&) {
  return nullptr;
}

std::unique_ptr<api::CredentialStorage>
ImplementationPlatform::CreateCredentialStorage() {
  return nullptr;
}

std::unique_ptr<WifiMedium> ImplementationPlatform::CreateWifiMedium() {
  return std::make_unique<linux_platform::WifiMediumImpl>();
}

std::unique_ptr<WifiLanMedium>
ImplementationPlatform::CreateWifiLanMedium() {
  return linux_platform::CreateWifiLanMedium();
}

std::unique_ptr<WifiAwareMedium>
ImplementationPlatform::CreateWifiAwareMedium() {
  return nullptr;
}

std::unique_ptr<WifiHotspotMedium>
ImplementationPlatform::CreateWifiHotspotMedium() {
  return nullptr;
}

std::unique_ptr<WifiDirectMedium>
ImplementationPlatform::CreateWifiDirectMedium() {
  return nullptr;
}

std::unique_ptr<Timer> ImplementationPlatform::CreateTimer() {
  static std::unique_ptr<ScheduledExecutor> executor =
      ImplementationPlatform::CreateScheduledExecutor();
  return std::make_unique<linux_platform::TimerImpl>(executor.get());
}

std::unique_ptr<AppLifecycleMonitor>
ImplementationPlatform::CreateAppLifecycleMonitor(
    std::function<void(AppLifecycleMonitor::AppLifecycleState)>
        state_updated_callback) {
  return std::make_unique<linux_platform::AppLifecycleMonitorImpl>(
      std::move(state_updated_callback));
}

std::unique_ptr<PreferencesManager>
ImplementationPlatform::CreatePreferencesManager(absl::string_view path) {
  return nullptr;
}

std::unique_ptr<DeviceInfo> ImplementationPlatform::CreateDeviceInfo() {
  return std::make_unique<linux_platform::DeviceInfoImpl>();
}

absl::StatusOr<WebResponse> ImplementationPlatform::SendRequest(
    const WebRequest& request) {
  return absl::UnimplementedError("HTTP requests are not implemented");
}

}  // namespace api

ByteArray Crypto::Sha256(absl::string_view input) {
  unsigned char digest[SHA256_DIGEST_LENGTH];
  SHA256(reinterpret_cast<const unsigned char*>(input.data()), input.size(),
         digest);
  return ByteArray(reinterpret_cast<const char*>(digest), sizeof(digest));
}

absl::Time SystemClock::ElapsedRealtime() { return absl::Now(); }

Exception SystemClock::Sleep(absl::Duration duration) {
  absl::SleepFor(duration);
  return {Exception::kSuccess};
}

}  // namespace nearby
