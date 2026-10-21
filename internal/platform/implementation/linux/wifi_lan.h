#ifndef PLATFORM_IMPL_LINUX_WIFI_LAN_H_
#define PLATFORM_IMPL_LINUX_WIFI_LAN_H_

#include <memory>

#include "internal/platform/implementation/wifi_lan.h"

namespace nearby {
namespace linux_platform {

std::unique_ptr<api::WifiLanMedium> CreateWifiLanMedium();

}  // namespace linux_platform
}  // namespace nearby

#endif  // PLATFORM_IMPL_LINUX_WIFI_LAN_H_
