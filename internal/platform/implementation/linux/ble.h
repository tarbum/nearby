#ifndef PLATFORM_IMPL_LINUX_BLE_H_
#define PLATFORM_IMPL_LINUX_BLE_H_

#include <memory>

#include "internal/platform/implementation/ble.h"
#include "internal/platform/implementation/bluetooth_adapter.h"

namespace nearby {
namespace linux_platform {

std::unique_ptr<api::BluetoothAdapter> CreateBluetoothAdapter();
std::unique_ptr<api::ble::BleMedium> CreateBleMedium();

}  // namespace linux_platform
}  // namespace nearby

#endif  // PLATFORM_IMPL_LINUX_BLE_H_
