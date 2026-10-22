#include "internal/platform/implementation/linux/ble.h"

#include <bluetooth/bluetooth.h>
#include <bluetooth/l2cap.h>
#include <gio/gio.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cerrno>
#include <deque>
#include <tuple>
#include <condition_variable>
#include <cstring>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include "absl/functional/any_invocable.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/escaping.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/string_view.h"
#include "internal/platform/byte_array.h"
#include "internal/platform/cancellation_flag.h"
#include "internal/platform/exception.h"
#include "internal/platform/implementation/ble.h"
#include "internal/platform/logging.h"
#include "internal/platform/mac_address.h"
#include "internal/platform/uuid.h"
#include "internal/weave/connection.h"
#include "internal/weave/socket_callback.h"
#include "internal/weave/sockets/client_socket.h"
#include "internal/weave/sockets/server_socket.h"

namespace nearby {
namespace linux_platform {
namespace {

using ::nearby::api::ble::AdvertiseParameters;
using ::nearby::api::ble::BleAdvertisementData;
using ::nearby::api::ble::BleL2capSocket;
using ::nearby::api::ble::BlePeripheral;
using ::nearby::api::ble::ClientGattConnectionCallback;
using ::nearby::api::ble::GattCharacteristic;
using ::nearby::api::ble::GattClient;
using ::nearby::api::ble::GattServer;
using ::nearby::api::ble::ServerGattConnectionCallback;
using ::nearby::api::ble::TxPowerLevel;

constexpr char kBlueZService[] = "org.bluez";
constexpr char kAdapterInterface[] = "org.bluez.Adapter1";
std::string g_gatt_slot_value;
std::mutex g_gatt_slot_mutex;
constexpr char kAdapterObject[] = "/org/bluez/hci0";
constexpr char kAdvertisingInterface[] = "org.bluez.LEAdvertisingManager1";
constexpr char kAdvertisementInterface[] = "org.bluez.LEAdvertisement1";
constexpr int kDBusTimeoutMs = 3000;

std::string VariantToString(GVariant* value) {
  if (value == nullptr) return "";
  const gchar* text = nullptr;
  if (g_variant_is_of_type(value, G_VARIANT_TYPE_STRING)) {
    text = g_variant_get_string(value, nullptr);
  } else if (g_variant_is_of_type(value, G_VARIANT_TYPE_OBJECT_PATH)) {
    text = g_variant_get_string(value, nullptr);
  }
  return text == nullptr ? "" : text;
}

bool VariantToBool(GVariant* value) {
  return value != nullptr && g_variant_is_of_type(value, G_VARIANT_TYPE_BOOLEAN)
             ? g_variant_get_boolean(value)
             : false;
}

std::optional<std::string> GetProperty(GDBusConnection* connection,
                                       const std::string& path,
                                       const char* interface, const char* name) {
  GError* error = nullptr;
  GVariant* reply = g_dbus_connection_call_sync(
      connection, kBlueZService, path.c_str(), "org.freedesktop.DBus.Properties",
      "Get", g_variant_new("(ss)", interface, name), G_VARIANT_TYPE("(v)"),
      G_DBUS_CALL_FLAGS_NONE, kDBusTimeoutMs, nullptr, &error);
  if (reply == nullptr) {
    if (error != nullptr) {
      LOG(WARNING) << "BlueZ property " << interface << "." << name
                   << " failed: " << error->message;
      g_error_free(error);
    }
    return std::nullopt;
  }
  GVariant* value = nullptr;
  g_variant_get(reply, "(v)", &value);
  std::optional<std::string> result;
  if (value != nullptr &&
      (g_variant_is_of_type(value, G_VARIANT_TYPE_STRING) ||
       g_variant_is_of_type(value, G_VARIANT_TYPE_OBJECT_PATH))) {
    result = VariantToString(value);
  }
  if (value != nullptr) g_variant_unref(value);
  g_variant_unref(reply);
  return result;
}

std::optional<bool> GetBoolProperty(GDBusConnection* connection,
                                    const std::string& path,
                                    const char* interface,
                                    const char* name) {
  GError* error = nullptr;
  GVariant* reply = g_dbus_connection_call_sync(
      connection, kBlueZService, path.c_str(), "org.freedesktop.DBus.Properties",
      "Get", g_variant_new("(ss)", interface, name), G_VARIANT_TYPE("(v)"),
      G_DBUS_CALL_FLAGS_NONE, kDBusTimeoutMs, nullptr, &error);
  if (reply == nullptr) {
    if (error != nullptr) g_error_free(error);
    return std::nullopt;
  }
  GVariant* value = nullptr;
  g_variant_get(reply, "(v)", &value);
  std::optional<bool> result;
  if (value != nullptr && g_variant_is_of_type(value, G_VARIANT_TYPE_BOOLEAN)) {
    result = g_variant_get_boolean(value);
  }
  if (value != nullptr) g_variant_unref(value);
  g_variant_unref(reply);
  return result;
}

bool SetProperty(GDBusConnection* connection, const std::string& path,
                 const char* interface, const char* name, GVariant* value) {
  GError* error = nullptr;
  GVariant* reply = g_dbus_connection_call_sync(
      connection, kBlueZService, path.c_str(), "org.freedesktop.DBus.Properties",
      "Set", g_variant_new("(ssv)", interface, name, value), nullptr,
      G_DBUS_CALL_FLAGS_NONE, kDBusTimeoutMs, nullptr, &error);
  if (reply == nullptr) {
    if (error != nullptr) {
      LOG(WARNING) << "BlueZ set " << interface << "." << name
                   << " failed: " << error->message;
      g_error_free(error);
    }
    return false;
  }
  g_variant_unref(reply);
  return true;
}

bool CallMethod(GDBusConnection* connection, const std::string& path,
                const char* interface, const char* method, GVariant* parameters,
                const GVariantType* reply_type = nullptr) {
  GError* error = nullptr;
  GVariant* reply = g_dbus_connection_call_sync(
      connection, kBlueZService, path.c_str(), interface, method, parameters,
      reply_type, G_DBUS_CALL_FLAGS_NONE, kDBusTimeoutMs, nullptr, &error);
  if (reply == nullptr) {
    if (error != nullptr) {
      LOG(WARNING) << "BlueZ call " << interface << "." << method
                   << " failed: " << error->message;
      g_error_free(error);
    }
    return false;
  }
  g_variant_unref(reply);
  return true;
}

class BluezConnection {
 public:
  static BluezConnection* Get() {
    static BluezConnection* connection = new BluezConnection();
    return connection;
  }

  GDBusConnection* connection() const { return connection_; }
  const std::string& adapter_path() const { return adapter_path_; }
  bool available() const { return connection_ != nullptr; }

 private:
  BluezConnection() {
    context_ = g_main_context_new();
    g_main_context_push_thread_default(context_);
    GError* error = nullptr;
    connection_ = g_bus_get_sync(G_BUS_TYPE_SYSTEM, nullptr, &error);
    g_main_context_pop_thread_default(context_);
    if (connection_ == nullptr) {
      LOG(WARNING) << "Could not connect to the system D-Bus";
      if (error != nullptr) {
        LOG(WARNING) << error->message;
        g_error_free(error);
      }
      g_main_context_unref(context_);
      context_ = nullptr;
      return;
    }

    loop_ = g_main_loop_new(context_, FALSE);
    thread_ = std::thread([this]() { g_main_loop_run(loop_); });
    adapter_path_ = FindAdapterPath();
  }

  ~BluezConnection() {
    if (loop_ != nullptr) g_main_loop_quit(loop_);
    if (thread_.joinable()) thread_.join();
    if (connection_ != nullptr) g_object_unref(connection_);
    if (loop_ != nullptr) g_main_loop_unref(loop_);
    if (context_ != nullptr) g_main_context_unref(context_);
  }

  std::string FindAdapterPath() {
    GError* error = nullptr;
    GVariant* reply = g_dbus_connection_call_sync(
        connection_, kBlueZService, "/",
        "org.freedesktop.DBus.ObjectManager", "GetManagedObjects", nullptr,
        G_VARIANT_TYPE("(a{oa{sa{sv}}})"), G_DBUS_CALL_FLAGS_NONE,
        kDBusTimeoutMs, nullptr, &error);
    if (reply == nullptr) {
      if (error != nullptr) {
        LOG(WARNING) << "BlueZ GetManagedObjects failed: " << error->message;
        g_error_free(error);
      }
      return kAdapterObject;
    }
    GVariant* objects = nullptr;
    g_variant_get(reply, "(@a{oa{sa{sv}}})", &objects);
    std::string result;
    GVariantIter iter;
    g_variant_iter_init(&iter, objects);
    const gchar* path = nullptr;
    GVariant* interfaces = nullptr;
    while (g_variant_iter_next(&iter, "{&o@a{sa{sv}}}", &path, &interfaces)) {
      GVariant* adapter = g_variant_lookup_value(
          interfaces, kAdapterInterface, G_VARIANT_TYPE("a{sv}"));
      if (adapter != nullptr) {
        result = path;
        g_variant_unref(adapter);
        g_variant_unref(interfaces);
        break;
      }
      g_variant_unref(interfaces);
    }
    g_variant_unref(objects);
    g_variant_unref(reply);
    return result.empty() ? kAdapterObject : result;
  }

  GMainContext* context_ = nullptr;
  GMainLoop* loop_ = nullptr;
  GDBusConnection* connection_ = nullptr;
  std::thread thread_;
  std::string adapter_path_ = kAdapterObject;
};

class BluetoothAdapterImpl final : public api::BluetoothAdapter {
 public:
  bool SetStatus(Status status) override {
    if (!BluezConnection::Get()->available()) return false;
    return SetProperty(BluezConnection::Get()->connection(),
                       BluezConnection::Get()->adapter_path(), kAdapterInterface,
                       "Powered",
                       g_variant_new_boolean(status == Status::kEnabled));
  }

  bool IsEnabled() const override {
    if (!BluezConnection::Get()->available()) return false;
    auto powered = GetBoolProperty(BluezConnection::Get()->connection(),
                                   BluezConnection::Get()->adapter_path(),
                                   kAdapterInterface, "Powered");
    return powered.value_or(false);
  }

  ScanMode GetScanMode() const override {
    if (!IsEnabled()) return ScanMode::kNone;
    auto discoverable = GetBoolProperty(BluezConnection::Get()->connection(),
                                        BluezConnection::Get()->adapter_path(),
                                        kAdapterInterface, "Discoverable");
    return discoverable.value_or(false) ? ScanMode::kConnectableDiscoverable
                                        : ScanMode::kConnectable;
  }

  bool SetScanMode(ScanMode scan_mode) override {
    if (!BluezConnection::Get()->available()) return false;
    auto* connection = BluezConnection::Get()->connection();
    const auto& path = BluezConnection::Get()->adapter_path();
    if (!SetProperty(connection, path, kAdapterInterface, "Powered",
                     g_variant_new_boolean(true))) {
      return false;
    }
    return SetProperty(
        connection, path, kAdapterInterface, "Discoverable",
        g_variant_new_boolean(scan_mode == ScanMode::kConnectableDiscoverable));
  }

  std::string GetName() const override {
    if (!BluezConnection::Get()->available()) return "";
    auto alias = GetProperty(BluezConnection::Get()->connection(),
                             BluezConnection::Get()->adapter_path(),
                             kAdapterInterface, "Alias");
    if (alias.has_value() && !alias->empty()) return *alias;
    auto name = GetProperty(BluezConnection::Get()->connection(),
                            BluezConnection::Get()->adapter_path(),
                            kAdapterInterface, "Name");
    return name.value_or("");
  }

  bool SetName(absl::string_view name, bool persist) override {
    if (!BluezConnection::Get()->available()) return false;
    std::string value(name);
    return SetProperty(BluezConnection::Get()->connection(),
                       BluezConnection::Get()->adapter_path(), kAdapterInterface,
                       "Alias", g_variant_new_string(value.c_str()));
  }

  MacAddress GetMacAddress() const override {
    MacAddress result;
    if (!BluezConnection::Get()->available()) return result;
    auto address = GetProperty(BluezConnection::Get()->connection(),
                               BluezConnection::Get()->adapter_path(),
                               kAdapterInterface, "Address");
    if (address.has_value()) {
      MacAddress::FromString(*address, result);
    }
    return result;
  }
};


class GattServerImpl final : public GattServer {
 public:
  GattServerImpl() = default;
  ~GattServerImpl() override { Stop(); }

  std::optional<GattCharacteristic> CreateCharacteristic(
      const Uuid& service_uuid, const Uuid& characteristic_uuid,
      GattCharacteristic::Permission permission,
      GattCharacteristic::Property property,
      const ByteArray& initial_value) override {
    GattCharacteristic characteristic;
    characteristic.uuid = characteristic_uuid;
    characteristic.service_uuid = service_uuid;
    characteristic.permission = permission;
    characteristic.property = property;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (service_uuid_.IsEmpty()) {
        service_uuid_ = service_uuid;
        service_path_ = "/com/tarbum/quickshare/gatt/service0";
      }
      if (service_uuid_ != service_uuid) return std::nullopt;
      Entry entry;
      entry.characteristic = characteristic;
      entry.path = absl::StrCat(service_path_, "/char", entries_.size());
      entry.value = initial_value;
      entries_.push_back(std::move(entry));
    }
    if (!registered_) RegisterApplication();
    return characteristic;
  }

  bool UpdateCharacteristic(const GattCharacteristic& characteristic,
                            const ByteArray& value) override {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& entry : entries_) {
      if (entry.characteristic.uuid == characteristic.uuid) {
        entry.value = value;
        return true;
      }
    }
    return false;
  }

  absl::Status NotifyCharacteristicChanged(
      const GattCharacteristic& characteristic, bool confirm,
      const ByteArray& new_value) override {
    if (!UpdateCharacteristic(characteristic, new_value)) {
      return absl::NotFoundError("Characteristic not found");
    }
    return absl::OkStatus();
  }

  void Stop() override {
    std::vector<guint> registrations;
    guint app_registration = 0;
    bool registered = false;
    std::string app_path;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (!registered_) return;
      registered = true;
      registered_ = false;
      app_registration = app_registration_;
      app_registration_ = 0;
      registrations = std::move(char_registrations_);
      char_registrations_.clear();
      if (service_registration_ != 0) {
        registrations.push_back(service_registration_);
        service_registration_ = 0;
      }
    }
    if (registered && BluezConnection::Get()->available()) {
      CallMethod(BluezConnection::Get()->connection(),
                 BluezConnection::Get()->adapter_path(), "org.bluez.GattManager1",
                 "UnregisterApplication",
                 g_variant_new("(o)", "/com/tarbum/quickshare/gatt"));
      for (guint id : registrations) {
        g_dbus_connection_unregister_object(
            BluezConnection::Get()->connection(), id);
      }
      if (app_registration != 0) {
        g_dbus_connection_unregister_object(
            BluezConnection::Get()->connection(), app_registration);
      }
    }
  }

 private:
  struct Entry {
    GattCharacteristic characteristic;
    std::string path;
    ByteArray value;
  };

  static GVariant* BuildBytes(const ByteArray& value) {
    return g_variant_new_fixed_array(G_VARIANT_TYPE_BYTE, value.data(),
                                     value.size(), 1);
  }

  bool RegisterApplication() {
    std::string service_path;
    Uuid service_uuid;
    std::vector<Entry> entries;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (entries_.empty()) return false;
      service_path = service_path_;
      service_uuid = service_uuid_;
      entries = entries_;
    }

    static const char kAppXml[] =
        "<node>"
        "<interface name='org.freedesktop.DBus.ObjectManager'>"
        "<method name='GetManagedObjects'>"
        "<arg type='a{oa{sa{sv}}}' direction='out'/>"
        "</method>"
        "</interface>"
        "</node>";
    static const char kServiceXml[] =
        "<node>"
        "<interface name='org.bluez.GattService1'>"
        "<property name='UUID' type='s' access='read'/>"
        "<property name='Primary' type='b' access='read'/>"
        "<property name='Characteristics' type='ao' access='read'/>"
        "</interface>"
        "</node>";
    static const char kCharacteristicXml[] =
        "<node>"
        "<interface name='org.bluez.GattCharacteristic1'>"
        "<method name='ReadValue'>"
        "<arg type='a{sv}' direction='in'/>"
        "<arg type='ay' direction='out'/>"
        "</method>"
        "<method name='WriteValue'>"
        "<arg type='ay' direction='in'/>"
        "<arg type='a{sv}' direction='in'/>"
        "</method>"
        "<property name='UUID' type='s' access='read'/>"
        "<property name='Service' type='o' access='read'/>"
        "<property name='Flags' type='as' access='read'/>"
        "<property name='Value' type='ay' access='read'/>"
        "</interface>"
        "</node>";

    GDBusNodeInfo* app_node = g_dbus_node_info_new_for_xml(kAppXml, nullptr);
    GDBusNodeInfo* service_node =
        g_dbus_node_info_new_for_xml(kServiceXml, nullptr);
    GDBusNodeInfo* char_node =
        g_dbus_node_info_new_for_xml(kCharacteristicXml, nullptr);
    if (app_node == nullptr || service_node == nullptr || char_node == nullptr) {
      if (app_node != nullptr) g_dbus_node_info_unref(app_node);
      if (service_node != nullptr) g_dbus_node_info_unref(service_node);
      if (char_node != nullptr) g_dbus_node_info_unref(char_node);
      return false;
    }

    static const GDBusInterfaceVTable app_vtable = {
        .method_call = OnMethodCall,
        .get_property = nullptr,
        .set_property = nullptr,
    };
    static const GDBusInterfaceVTable service_vtable = {
        .method_call = nullptr,
        .get_property = OnGetProperty,
        .set_property = nullptr,
    };
    static const GDBusInterfaceVTable char_vtable = {
        .method_call = OnMethodCall,
        .get_property = OnGetProperty,
        .set_property = nullptr,
    };

    GError* error = nullptr;
    guint app_registration = g_dbus_connection_register_object(
        BluezConnection::Get()->connection(), "/com/tarbum/quickshare/gatt",
        g_dbus_node_info_lookup_interface(app_node,
                                          "org.freedesktop.DBus.ObjectManager"),
        &app_vtable, this, nullptr, &error);
    if (app_registration == 0) {
      if (error != nullptr) g_error_free(error);
      g_dbus_node_info_unref(app_node);
      g_dbus_node_info_unref(service_node);
      g_dbus_node_info_unref(char_node);
      return false;
    }
    guint service_registration = g_dbus_connection_register_object(
        BluezConnection::Get()->connection(), service_path.c_str(),
        g_dbus_node_info_lookup_interface(service_node,
                                          "org.bluez.GattService1"),
        &service_vtable, this, nullptr, &error);
    std::vector<guint> char_registrations;
    for (const auto& entry : entries) {
      guint registration = g_dbus_connection_register_object(
          BluezConnection::Get()->connection(), entry.path.c_str(),
          g_dbus_node_info_lookup_interface(char_node,
                                            "org.bluez.GattCharacteristic1"),
          &char_vtable, this, nullptr, &error);
      if (registration != 0) char_registrations.push_back(registration);
    }
    g_dbus_node_info_unref(app_node);
    g_dbus_node_info_unref(service_node);
    g_dbus_node_info_unref(char_node);

    GVariantBuilder options;
    g_variant_builder_init(&options, G_VARIANT_TYPE("a{sv}"));
    bool ok = service_registration != 0 &&
              char_registrations.size() == entries.size() &&
              CallMethod(BluezConnection::Get()->connection(),
                         BluezConnection::Get()->adapter_path(),
                         "org.bluez.GattManager1", "RegisterApplication",
                         g_variant_new("(o@a{sv})",
                                       "/com/tarbum/quickshare/gatt",
                                       g_variant_builder_end(&options)));
    if (!ok) {
      if (error != nullptr) g_error_free(error);
      g_dbus_connection_unregister_object(BluezConnection::Get()->connection(),
                                          app_registration);
      if (service_registration != 0) {
        g_dbus_connection_unregister_object(
            BluezConnection::Get()->connection(), service_registration);
      }
      for (guint id : char_registrations) {
        g_dbus_connection_unregister_object(
            BluezConnection::Get()->connection(), id);
      }
      return false;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    app_registration_ = app_registration;
    service_registration_ = service_registration;
    char_registrations_ = std::move(char_registrations);
    registered_ = true;
    return true;
  }

  static void OnMethodCall(GDBusConnection* connection, const gchar* sender,
                           const gchar* object_path,
                           const gchar* interface_name,
                           const gchar* method_name, GVariant* parameters,
                           GDBusMethodInvocation* invocation,
                           gpointer user_data) {
    auto* server = static_cast<GattServerImpl*>(user_data);
    if (std::strcmp(interface_name,
                    "org.freedesktop.DBus.ObjectManager") == 0 &&
        std::strcmp(method_name, "GetManagedObjects") == 0) {
      g_dbus_method_invocation_return_value(
          invocation,
          g_variant_new("(@a{oa{sa{sv}}})", server->BuildManagedObjects()));
      return;
    }
    if (std::strcmp(interface_name, "org.bluez.GattCharacteristic1") == 0) {
      if (std::strcmp(method_name, "ReadValue") == 0) {
        auto value = server->GetValue(object_path);
        if (!value.has_value()) {
          g_dbus_method_invocation_return_dbus_error(
              invocation, "org.bluez.Error.Failed", "Unknown characteristic");
          return;
        }
        g_dbus_method_invocation_return_value(
            invocation, g_variant_new("(@ay)", BuildBytes(*value)));
        return;
      }
      if (std::strcmp(method_name, "WriteValue") == 0) {
        GVariant* bytes = nullptr;
        g_variant_get(parameters, "(@ay@a{sv})", &bytes, nullptr);
        if (bytes != nullptr) {
          gsize length = 0;
          const auto* data = static_cast<const char*>(
              g_variant_get_fixed_array(bytes, &length, sizeof(char)));
          if (data != nullptr) {
            server->SetValue(object_path, ByteArray(data, length));
          }
          g_variant_unref(bytes);
        }
        g_dbus_method_invocation_return_value(invocation, nullptr);
        return;
      }
    }
    g_dbus_method_invocation_return_dbus_error(
        invocation, "org.bluez.Error.NotSupported", "Unsupported method");
  }

  static GVariant* OnGetProperty(GDBusConnection* connection,
                                 const gchar* sender,
                                 const gchar* object_path,
                                 const gchar* interface_name,
                                 const gchar* property_name, GError** error,
                                 gpointer user_data) {
    auto* server = static_cast<GattServerImpl*>(user_data);
    return server->GetProperty(object_path, interface_name, property_name);
  }

  GVariant* GetProperty(const gchar* object_path, const gchar* interface_name,
                        const gchar* property_name) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (std::strcmp(interface_name, "org.bluez.GattService1") == 0) {
      if (std::strcmp(property_name, "UUID") == 0) {
        return g_variant_new_string(static_cast<std::string>(service_uuid_).c_str());
      }
      if (std::strcmp(property_name, "Primary") == 0) {
        return g_variant_new_boolean(true);
      }
      if (std::strcmp(property_name, "Characteristics") == 0) {
        GVariantBuilder builder;
        g_variant_builder_init(&builder, G_VARIANT_TYPE("ao"));
        for (const auto& entry : entries_) {
          g_variant_builder_add(&builder, "o", entry.path.c_str());
        }
        return g_variant_builder_end(&builder);
      }
    }
    if (std::strcmp(interface_name, "org.bluez.GattCharacteristic1") == 0) {
      for (const auto& entry : entries_) {
        if (entry.path != object_path) continue;
        if (std::strcmp(property_name, "UUID") == 0) {
          return g_variant_new_string(
              static_cast<std::string>(entry.characteristic.uuid).c_str());
        }
        if (std::strcmp(property_name, "Service") == 0) {
          return g_variant_new_object_path(service_path_.c_str());
        }
        if (std::strcmp(property_name, "Flags") == 0) {
          GVariantBuilder builder;
          g_variant_builder_init(&builder, G_VARIANT_TYPE("as"));
          g_variant_builder_add(&builder, "s", "read");
          return g_variant_builder_end(&builder);
        }
        if (std::strcmp(property_name, "Value") == 0) {
          return BuildBytes(entry.value);
        }
      }
    }
    return nullptr;
  }

  GVariant* BuildManagedObjects() {
    std::lock_guard<std::mutex> lock(mutex_);
    GVariantBuilder objects;
    g_variant_builder_init(&objects, G_VARIANT_TYPE("a{oa{sa{sv}}}"));
    GVariantBuilder service_interfaces;
    g_variant_builder_init(&service_interfaces, G_VARIANT_TYPE("a{sa{sv}}"));
    GVariantBuilder service_props;
    g_variant_builder_init(&service_props, G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(&service_props, "{sv}", "UUID",
                          g_variant_new_string(
                              static_cast<std::string>(service_uuid_).c_str()));
    g_variant_builder_add(&service_props, "{sv}", "Primary",
                          g_variant_new_boolean(true));
    GVariantBuilder characteristics;
    g_variant_builder_init(&characteristics, G_VARIANT_TYPE("ao"));
    for (const auto& entry : entries_) {
      g_variant_builder_add(&characteristics, "o", entry.path.c_str());
    }
    g_variant_builder_add(&service_props, "{sv}", "Characteristics",
                          g_variant_builder_end(&characteristics));
    g_variant_builder_add(&service_interfaces, "{s@a{sv}}",
                          "org.bluez.GattService1",
                          g_variant_builder_end(&service_props));
    g_variant_builder_add(&objects, "{o@a{sa{sv}}}", service_path_.c_str(),
                          g_variant_builder_end(&service_interfaces));

    for (const auto& entry : entries_) {
      GVariantBuilder interfaces;
      g_variant_builder_init(&interfaces, G_VARIANT_TYPE("a{sa{sv}}"));
      GVariantBuilder props;
      g_variant_builder_init(&props, G_VARIANT_TYPE("a{sv}"));
      g_variant_builder_add(
          &props, "{sv}", "UUID",
          g_variant_new_string(
              static_cast<std::string>(entry.characteristic.uuid).c_str()));
      g_variant_builder_add(&props, "{sv}", "Service",
                            g_variant_new_object_path(service_path_.c_str()));
      GVariantBuilder flags;
      g_variant_builder_init(&flags, G_VARIANT_TYPE("as"));
      g_variant_builder_add(&flags, "s", "read");
      g_variant_builder_add(&props, "{sv}", "Flags",
                            g_variant_builder_end(&flags));
      g_variant_builder_add(&props, "{sv}", "Value", BuildBytes(entry.value));
      g_variant_builder_add(&interfaces, "{s@a{sv}}",
                            "org.bluez.GattCharacteristic1",
                            g_variant_builder_end(&props));
      g_variant_builder_add(&objects, "{o@a{sa{sv}}}", entry.path.c_str(),
                            g_variant_builder_end(&interfaces));
    }
    return g_variant_builder_end(&objects);
  }

  std::optional<ByteArray> GetValue(const std::string& path) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& entry : entries_) {
      if (entry.path == path) return entry.value;
    }
    return std::nullopt;
  }

  void SetValue(const std::string& path, const ByteArray& value) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& entry : entries_) {
      if (entry.path == path) {
        entry.value = value;
        return;
      }
    }
  }

  std::mutex mutex_;
  bool registered_ = false;
  Uuid service_uuid_;
  std::string service_path_;
  std::vector<Entry> entries_;
  guint app_registration_ = 0;
  guint service_registration_ = 0;
  std::vector<guint> char_registrations_;
};

class DummyBleServerSocket final : public api::ble::BleServerSocket {
 public:
  std::unique_ptr<api::ble::BleSocket> Accept() override {
    std::unique_lock<std::mutex> lock(mutex_);
    condition_.wait(lock, [this]() { return closed_; });
    return nullptr;
  }

  Exception Close() override {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      closed_ = true;
    }
    condition_.notify_all();
    return {Exception::kSuccess};
  }

 private:
  std::mutex mutex_;
  std::condition_variable condition_;
  bool closed_ = false;
};

class BleMediumImpl final : public api::ble::BleMedium {
 public:
  BleMediumImpl() {
    if (!BluezConnection::Get()->available()) {
      LOG(WARNING) << "BLE unavailable: no system D-Bus connection";
      return;
    }
    added_subscription_ = g_dbus_connection_signal_subscribe(
        BluezConnection::Get()->connection(), nullptr,
        "org.freedesktop.DBus.ObjectManager", "InterfacesAdded", "/", nullptr,
        G_DBUS_SIGNAL_FLAGS_NONE, OnInterfacesAdded, this, nullptr);
    changed_subscription_ = g_dbus_connection_signal_subscribe(
        BluezConnection::Get()->connection(), nullptr,
        "org.freedesktop.DBus.Properties", "PropertiesChanged", nullptr, nullptr,
        G_DBUS_SIGNAL_FLAGS_NONE, OnPropertiesChanged, this, nullptr);
  }

  ~BleMediumImpl() override {
    StopAdvertising();
    StopScanning();
    if (BluezConnection::Get()->available()) {
      g_dbus_connection_signal_unsubscribe(BluezConnection::Get()->connection(),
                                           added_subscription_);
      g_dbus_connection_signal_unsubscribe(BluezConnection::Get()->connection(),
                                           changed_subscription_);
    }
  }

  bool StartAdvertising(const BleAdvertisementData& advertising_data,
                        AdvertiseParameters advertise_parameters) override {
    return StartAdvertisingInternal(advertising_data, advertise_parameters);
  }

  bool StopAdvertising() override {
    std::string advertising_path;
    guint registration_id = 0;
    {
      std::lock_guard<std::mutex> lock(advertising_mutex_);
      if (!advertising_) return true;
      advertising_ = false;
      advertising_path = advertising_path_;
      advertising_path_.clear();
      registration_id = registration_id_;
      registration_id_ = 0;
      advertising_data_.service_data.clear();
    }
    if (BluezConnection::Get()->available()) {
      if (!advertising_path.empty()) {
        CallMethod(BluezConnection::Get()->connection(),
                   BluezConnection::Get()->adapter_path(), kAdvertisingInterface,
                   "UnregisterAdvertisement",
                   g_variant_new("(o)", advertising_path.c_str()));
      }
      if (registration_id != 0) {
        g_dbus_connection_unregister_object(
            BluezConnection::Get()->connection(), registration_id);
      }
    }
    return true;
  }

  std::unique_ptr<AdvertisingSession> StartAdvertising(
      const BleAdvertisementData& advertising_data,
      AdvertiseParameters advertise_parameters,
      AdvertisingCallback callback) override {
    bool result = StartAdvertisingInternal(advertising_data, advertise_parameters);
    if (callback.start_advertising_result) {
      callback.start_advertising_result(
          result ? absl::OkStatus()
                 : absl::InternalError("BlueZ advertising failed"));
    }
    return std::make_unique<AdvertisingSession>(
        AdvertisingSession{.stop_advertising = [this]() {
          return StopAdvertising()
                     ? absl::OkStatus()
                     : absl::InternalError("Failed to stop advertising");
        }});
  }

  bool StartScanning(const Uuid& service_uuid, TxPowerLevel tx_power_level,
                     ScanCallback callback) override {
    return StartScanningInternal({service_uuid}, std::move(callback));
  }

  bool StartMultipleServicesScanning(
      const std::vector<Uuid>& service_uuids, TxPowerLevel tx_power_level,
      ScanCallback callback) override {
    return StartScanningInternal(service_uuids, std::move(callback));
  }

  bool StopScanning() override {
    {
      std::lock_guard<std::mutex> lock(scanning_mutex_);
      if (!scanning_) return true;
      scanning_ = false;
      scan_callback_.reset();
    }
    poll_running_.store(false);
    poll_condition_.notify_all();
    if (poll_thread_.joinable()) poll_thread_.join();
    if (BluezConnection::Get()->available()) {
      CallMethod(BluezConnection::Get()->connection(),
                 BluezConnection::Get()->adapter_path(), kAdapterInterface,
                 "StopDiscovery", nullptr);
    }
    return true;
  }

  std::unique_ptr<ScanningSession> StartScanning(
      const Uuid& service_uuid, TxPowerLevel tx_power_level,
      ScanningCallback callback) override {
    auto callback_ptr =
        std::make_shared<ScanningCallback>(std::move(callback));
    bool started = StartScanningInternal(
        {service_uuid},
        ScanCallback{.advertisement_found_cb =
                         [callback_ptr](BlePeripheral::UniqueId id,
                                        BleAdvertisementData data) {
                           callback_ptr->advertisement_found_cb(id, data);
                         }});
    if (callback_ptr->start_scanning_result) {
      callback_ptr->start_scanning_result(
          started ? absl::OkStatus()
                  : absl::InternalError("BlueZ scanning failed"));
    }
    if (!started) return nullptr;
    return std::make_unique<ScanningSession>(
        ScanningSession{.stop_scanning = [this]() {
          return StopScanning()
                     ? absl::OkStatus()
                     : absl::InternalError("Failed to stop scanning");
        }});
  }

  bool PauseMediumScanning() override { return true; }
  bool ResumeMediumScanning() override { return true; }

  std::unique_ptr<GattServer> StartGattServer(
      ServerGattConnectionCallback callback) override {
    return std::make_unique<GattServerImpl>();
  }

  std::unique_ptr<GattClient> ConnectToGattServer(
      BlePeripheral::UniqueId peripheral_id, TxPowerLevel tx_power_level,
      ClientGattConnectionCallback callback) override;

  std::unique_ptr<BleL2capSocket> ConnectOverL2cap(
      int psm, const std::string& service_id, TxPowerLevel tx_power_level,
      BlePeripheral::UniqueId peripheral_id,
      CancellationFlag* cancellation_flag) override;

  std::unique_ptr<api::ble::BleServerSocket> OpenServerSocket(
      const std::string& service_id) override;

  std::unique_ptr<api::ble::BleSocket> Connect(
      const std::string& service_id, TxPowerLevel tx_power_level,
      BlePeripheral::UniqueId peripheral_id,
      CancellationFlag* cancellation_flag) override;

  bool IsExtendedAdvertisementsAvailable() override { return false; }

  std::optional<BlePeripheral::UniqueId> RetrieveBlePeripheralIdFromNativeId(
      const std::string& native_id) override {
    std::lock_guard<std::mutex> lock(peripherals_mutex_);
    auto it = address_to_id_.find(native_id);
    if (it == address_to_id_.end()) return std::nullopt;
    return it->second;
  }

 private:
  static void OnInterfacesAdded(GDBusConnection* connection,
                                const gchar* sender_name,
                                const gchar* object_path,
                                const gchar* interface_name,
                                const gchar* signal_name, GVariant* parameters,
                                gpointer user_data) {
    auto* medium = static_cast<BleMediumImpl*>(user_data);
    medium->HandleInterfacesAdded(parameters);
  }

  static void OnPropertiesChanged(GDBusConnection* connection,
                                  const gchar* sender_name,
                                  const gchar* object_path,
                                  const gchar* interface_name,
                                  const gchar* signal_name,
                                  GVariant* parameters, gpointer user_data) {
    auto* medium = static_cast<BleMediumImpl*>(user_data);
    medium->HandlePropertiesChanged(object_path, parameters);
  }

  static GVariant* OnGetProperty(GDBusConnection* connection,
                                 const gchar* sender,
                                 const gchar* object_path,
                                 const gchar* interface_name,
                                 const gchar* property_name, GError** error,
                                 gpointer user_data) {
    auto* medium = static_cast<BleMediumImpl*>(user_data);
    return medium->GetAdvertisementProperty(property_name);
  }

  static void OnMethodCall(GDBusConnection* connection, const gchar* sender,
                           const gchar* object_path,
                           const gchar* interface_name,
                           const gchar* method_name, GVariant* parameters,
                           GDBusMethodInvocation* invocation,
                           gpointer user_data) {
    if (std::strcmp(method_name, "Release") == 0) {
      g_dbus_method_invocation_return_value(invocation, nullptr);
      return;
    }
    g_dbus_method_invocation_return_dbus_error(
        invocation, "org.bluez.Error.NotSupported", "Unsupported method");
  }

  bool StartAdvertisingInternal(const BleAdvertisementData& data,
                                AdvertiseParameters parameters) {
    if (!BluezConnection::Get()->available()) return false;
    if (data.is_extended_advertisement) {
      LOG(INFO) << "BlueZ backend does not support extended advertising yet";
      return false;
    }
    if (data.service_data.empty()) return false;

    if (advertising_) StopAdvertising();
    std::string advertising_path;
    guint registration_id = 0;
    {
      std::lock_guard<std::mutex> lock(advertising_mutex_);
      advertising_data_ = data;
      advertising_connectable_ = parameters.is_connectable;
      advertising_path_ = absl::StrCat(
          "/com/tarbum/quickshare/adv", next_advertisement_id_++);
      advertising_path = advertising_path_;
    }
    {
      std::lock_guard<std::mutex> lock{g_gatt_slot_mutex};
      for (const auto& [uuid, value] : data.service_data) {
        if (std::string(uuid) == "0000fef3-0000-1000-8000-00805f9b34fb") {
          g_gatt_slot_value.assign(value.data(), value.size());
        }
      }
    }

    static const char kIntrospectionXml[] =
        "<node>"
        "<interface name='org.bluez.LEAdvertisement1'>"
        "<method name='Release'/>"
        "<property name='Type' type='s' access='read'/>"
        "<property name='ServiceUUIDs' type='as' access='read'/>"
        "<property name='ServiceData' type='a{sv}' access='read'/>"
        "<property name='Discoverable' type='b' access='read'/>"
        "<property name='IncludeTxPower' type='b' access='read'/>"
        "</interface>"
        "</node>";
    GDBusNodeInfo* node =
        g_dbus_node_info_new_for_xml(kIntrospectionXml, nullptr);
    if (node == nullptr) return false;
    GDBusInterfaceInfo* interface =
        g_dbus_node_info_lookup_interface(node, kAdvertisementInterface);
    if (interface == nullptr) {
      g_dbus_node_info_unref(node);
      return false;
    }
    static const GDBusInterfaceVTable vtable = {
        .method_call = OnMethodCall,
        .get_property = OnGetProperty,
        .set_property = nullptr,
    };
    GError* error = nullptr;
    registration_id = g_dbus_connection_register_object(
        BluezConnection::Get()->connection(), advertising_path.c_str(), interface,
        &vtable, this, nullptr, &error);
    g_dbus_node_info_unref(node);
    if (registration_id == 0) {
      if (error != nullptr) {
        LOG(WARNING) << "Could not register BLE advertisement object: "
                     << error->message;
        g_error_free(error);
      }
      std::lock_guard<std::mutex> lock(advertising_mutex_);
      advertising_path_.clear();
      return false;
    }

    GVariantBuilder options;
    g_variant_builder_init(&options, G_VARIANT_TYPE("a{sv}"));
    bool registered = CallMethod(
        BluezConnection::Get()->connection(),
        BluezConnection::Get()->adapter_path(), kAdvertisingInterface,
        "RegisterAdvertisement",
        g_variant_new("(o@a{sv})", advertising_path.c_str(),
                      g_variant_builder_end(&options)));
    if (!registered) {
      g_dbus_connection_unregister_object(BluezConnection::Get()->connection(),
                                          registration_id);
      std::lock_guard<std::mutex> lock(advertising_mutex_);
      advertising_path_.clear();
      return false;
    }
    std::lock_guard<std::mutex> lock(advertising_mutex_);
    registration_id_ = registration_id;
    advertising_ = true;
    return true;
  }

  GVariant* GetAdvertisementProperty(const gchar* property_name) {
    std::lock_guard<std::mutex> lock(advertising_mutex_);
    if (std::strcmp(property_name, "Type") == 0) {
      return g_variant_new_string(advertising_connectable_ ? "peripheral"
                                                           : "broadcast");
    }
    if (std::strcmp(property_name, "Discoverable") == 0) {
      return g_variant_new_boolean(false);
    }
    if (std::strcmp(property_name, "IncludeTxPower") == 0) {
      return g_variant_new_boolean(true);
    }
    if (std::strcmp(property_name, "ServiceUUIDs") == 0) {
      GVariantBuilder builder;
      g_variant_builder_init(&builder, G_VARIANT_TYPE("as"));
      for (const auto& [uuid, value] : advertising_data_.service_data) {
        std::string text = static_cast<std::string>(uuid);
        g_variant_builder_add(&builder, "s", text.c_str());
      }
      return g_variant_builder_end(&builder);
    }
    if (std::strcmp(property_name, "ServiceData") == 0) {
      GVariantBuilder builder;
      g_variant_builder_init(&builder, G_VARIANT_TYPE("a{sv}"));
      for (const auto& [uuid, value] : advertising_data_.service_data) {
        std::string text = static_cast<std::string>(uuid);
        GVariant* bytes = g_variant_new_fixed_array(
            G_VARIANT_TYPE_BYTE, value.data(), value.size(), 1);
        g_variant_builder_add(&builder, "{sv}", text.c_str(), bytes);
      }
      return g_variant_builder_end(&builder);
    }
    return nullptr;
  }

  bool StartScanningInternal(const std::vector<Uuid>& service_uuids,
                             ScanCallback callback) {
    if (!BluezConnection::Get()->available()) return false;
    if (scanning_) StopScanning();
    {
      std::lock_guard<std::mutex> lock(scanning_mutex_);
      scan_callback_ = std::make_shared<ScanCallback>(std::move(callback));
      scanning_ = true;
    }

    GVariantBuilder filter;
    g_variant_builder_init(&filter, G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(&filter, "{sv}", "Transport",
                          g_variant_new_string("le"));
    g_variant_builder_add(&filter, "{sv}", "DuplicateData",
                          g_variant_new_boolean(true));
    bool configured = CallMethod(
        BluezConnection::Get()->connection(),
        BluezConnection::Get()->adapter_path(), kAdapterInterface,
        "SetDiscoveryFilter",
        g_variant_new("(@a{sv})", g_variant_builder_end(&filter)));
    bool started = configured &&
                   CallMethod(BluezConnection::Get()->connection(),
                              BluezConnection::Get()->adapter_path(),
                              kAdapterInterface, "StartDiscovery", nullptr);
    if (!started) {
      std::lock_guard<std::mutex> lock(scanning_mutex_);
      scanning_ = false;
      scan_callback_.reset();
    } else {
      EnsurePolling();
    }
    return started;
  }

  void EnsurePolling() {
    if (poll_running_.exchange(true)) return;
    poll_thread_ = std::thread([this]() {
      while (poll_running_.load()) {
        PollDevices();
        std::unique_lock<std::mutex> lock(poll_mutex_);
        poll_condition_.wait_for(lock, std::chrono::seconds(1),
                                 [this]() { return !poll_running_.load(); });
      }
    });
  }

  void PollDevices() {
    if (!BluezConnection::Get()->available()) return;
    GError* error = nullptr;
    GVariant* reply = g_dbus_connection_call_sync(
        BluezConnection::Get()->connection(), kBlueZService, "/",
        "org.freedesktop.DBus.ObjectManager", "GetManagedObjects", nullptr,
        G_VARIANT_TYPE("(a{oa{sa{sv}}})"), G_DBUS_CALL_FLAGS_NONE,
        kDBusTimeoutMs, nullptr, &error);
    if (reply == nullptr) {
      if (error != nullptr) g_error_free(error);
      return;
    }
    GVariant* objects = nullptr;
    g_variant_get(reply, "(@a{oa{sa{sv}}})", &objects);
    GVariantIter iter;
    g_variant_iter_init(&iter, objects);
    const gchar* path = nullptr;
    GVariant* interfaces = nullptr;
    int devices = 0;
    while (g_variant_iter_next(&iter, "{&o@a{sa{sv}}}", &path, &interfaces)) {
      GVariant* properties = g_variant_lookup_value(
          interfaces, "org.bluez.Device1", G_VARIANT_TYPE("a{sv}"));
      if (properties != nullptr) {
        devices++;
        ProcessDeviceProperties(path, properties);
        g_variant_unref(properties);
      }
      g_variant_unref(interfaces);
    }
    g_variant_unref(objects);
    g_variant_unref(reply);
  }

  void HandleInterfacesAdded(GVariant* parameters) {
    const gchar* path = nullptr;
    GVariant* interfaces = nullptr;
    g_variant_get(parameters, "(&o@a{sa{sv}})", &path, &interfaces);
    GVariant* properties = g_variant_lookup_value(
        interfaces, "org.bluez.Device1", G_VARIANT_TYPE("a{sv}"));
    if (properties != nullptr) {
      ProcessDeviceProperties(path, properties);
      g_variant_unref(properties);
    }
    g_variant_unref(interfaces);
  }

  void HandlePropertiesChanged(const gchar* path, GVariant* parameters) {
    const gchar* interface = nullptr;
    GVariant* changed = nullptr;
    GVariant* invalidated = nullptr;
    g_variant_get(parameters, "(&s@a{sv}@as)", &interface, &changed,
                  &invalidated);
    if (std::strcmp(interface, "org.bluez.Device1") == 0) {
      ProcessDeviceProperties(path, changed);
    }
    g_variant_unref(changed);
    g_variant_unref(invalidated);
  }

  void ProcessDeviceProperties(const std::string& path, GVariant* properties) {
    GVariant* service_data = g_variant_lookup_value(
        properties, "ServiceData", G_VARIANT_TYPE("a{sv}"));

    BleAdvertisementData advertisement;
    advertisement.is_extended_advertisement = false;
    GVariantIter iter;
    const gchar* uuid_text = nullptr;
    GVariant* value = nullptr;
    if (service_data != nullptr) {
      g_variant_iter_init(&iter, service_data);
    }
    while (service_data != nullptr &&
           g_variant_iter_next(&iter, "{&s@v}", &uuid_text, &value)) {
      GVariant* bytes = nullptr;
      if (g_variant_is_of_type(value, G_VARIANT_TYPE_VARIANT)) {
        bytes = g_variant_get_variant(value);
      } else if (g_variant_is_of_type(value, G_VARIANT_TYPE("ay"))) {
        bytes = g_variant_ref(value);
      }
      if (bytes != nullptr &&
          g_variant_is_of_type(bytes, G_VARIANT_TYPE("ay"))) {
        gsize length = 0;
        const auto* data =
            static_cast<const char*>(g_variant_get_fixed_array(
                bytes, &length, sizeof(char)));
        std::string text(uuid_text);
        if (text.size() == 4) {
          text = "0000" + text + "-0000-1000-8000-00805f9b34fb";
        }
        auto uuid = Uuid::FromString(text);
        if (uuid.has_value() && data != nullptr && length > 0) {
          advertisement.service_data.emplace(*uuid,
                                             ByteArray(data, length));
        }
      }
      if (bytes != nullptr) g_variant_unref(bytes);
      g_variant_unref(value);
    }
    if (service_data != nullptr) g_variant_unref(service_data);

    if (advertisement.service_data.empty()) {
      GVariant* name_value =
          g_variant_lookup_value(properties, "Name", G_VARIANT_TYPE_STRING);
      if (name_value == nullptr) {
        name_value =
            g_variant_lookup_value(properties, "Alias", G_VARIANT_TYPE_STRING);
      }
      if (name_value != nullptr) {
        std::string name = VariantToString(name_value);
        g_variant_unref(name_value);
        GVariant* address_type = g_variant_lookup_value(
            properties, "AddressType", G_VARIANT_TYPE_STRING);
        bool random_address =
            address_type != nullptr &&
            VariantToString(address_type) == "random";
        if (address_type != nullptr) g_variant_unref(address_type);
        if (random_address && name.rfind("QS", 0) == 0) {
          while (name.size() % 4 != 0) name.push_back('=');
          std::string decoded;
          if (absl::Base64Unescape(name, &decoded) && decoded.size() >= 15 &&
              (static_cast<unsigned char>(decoded[0]) & 0xE0) == 0x40) {
            auto uuid = Uuid::FromString("0000fef3-0000-1000-8000-00805f9b34fb");
            if (uuid.has_value()) {
              advertisement.service_data.emplace(*uuid, ByteArray(decoded));
            }
          }
        }
      }
    }

    if (advertisement.service_data.empty()) return;
    LOG(INFO) << "BLE service data from " << path << " entries="
              << advertisement.service_data.size();

    GVariant* address_value = g_variant_lookup_value(
        properties, "Address", G_VARIANT_TYPE_STRING);
    std::string address;
    if (address_value != nullptr) {
      address = VariantToString(address_value);
      g_variant_unref(address_value);
    }
    if (address.empty()) {
      size_t marker = path.rfind("dev_");
      if (marker != std::string::npos) address = path.substr(marker + 4);
    }
    if (address.empty()) return;

    BlePeripheral::UniqueId id = 0;
    {
      std::lock_guard<std::mutex> lock(peripherals_mutex_);
      auto it = address_to_id_.find(address);
      if (it != address_to_id_.end()) {
        id = it->second;
      } else {
        id = next_peripheral_id_++;
        address_to_id_[address] = id;
        id_to_address_[id] = address;
      }
    }
    GVariant* address_type_value = g_variant_lookup_value(
        properties, "AddressType", G_VARIANT_TYPE_STRING);
    if (address_type_value != nullptr) {
      std::string address_type = VariantToString(address_type_value);
      g_variant_unref(address_type_value);
      std::lock_guard<std::mutex> lock(peripherals_mutex_);
      id_to_address_type_[id] = address_type == "random" ? 1 : 0;
    }

    std::shared_ptr<ScanCallback> callback;
    {
      std::lock_guard<std::mutex> lock(scanning_mutex_);
      if (!scanning_ || scan_callback_ == nullptr) return;
      callback = scan_callback_;
    }
    if (callback != nullptr) {
      LOG(INFO) << "BLE advertisement emitted for " << address;
      callback->advertisement_found_cb(id, std::move(advertisement));
    }
  }

  guint added_subscription_ = 0;
  guint changed_subscription_ = 0;
  std::mutex advertising_mutex_;
  bool advertising_ = false;
  bool advertising_connectable_ = true;
  std::string advertising_path_;
  guint registration_id_ = 0;
  uint32_t next_advertisement_id_ = 0;
  BleAdvertisementData advertising_data_;

  std::mutex scanning_mutex_;
  bool scanning_ = false;
  std::shared_ptr<ScanCallback> scan_callback_;
  std::atomic<bool> poll_running_{false};
  std::mutex poll_mutex_;
  std::condition_variable poll_condition_;
  std::thread poll_thread_;

  std::mutex peripherals_mutex_;
  std::unordered_map<std::string, BlePeripheral::UniqueId> address_to_id_;
  std::unordered_map<BlePeripheral::UniqueId, std::string> id_to_address_;
  std::unordered_map<BlePeripheral::UniqueId, uint8_t> id_to_address_type_;
  BlePeripheral::UniqueId next_peripheral_id_ = 1;
};

}  // namespace

std::unique_ptr<api::BluetoothAdapter> CreateBluetoothAdapter() {
  return std::make_unique<BluetoothAdapterImpl>();
}

std::unique_ptr<api::ble::BleMedium> CreateBleMedium() {
  return std::make_unique<BleMediumImpl>();
}

}  // namespace linux_platform
}  // namespace nearby

namespace nearby {
namespace linux_platform {

namespace weave = ::nearby::weave;

namespace {

constexpr char kWeaveServiceUuid[] =
    "0000fef3-0000-1000-8000-00805f9b34fb";
constexpr char kWeaveToPeripheralUuid[] =
    "00000100-0004-1000-8000-001a11000101";
constexpr char kWeaveFromPeripheralUuid[] =
    "00000100-0004-1000-8000-001a11000102";
constexpr char kAdvertisementSlotUuid[] =
    "00000000-0000-3000-8000-000000000000";
constexpr char kCccdUuid[] =
    "00002902-0000-1000-8000-00805f9b34fb";
constexpr int kWeavePacketSize = 512;

std::string Lowercase(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char c) { return std::tolower(c); });
  return value;
}

class WeaveInput final : public InputStream {
 public:
  void Push(std::string message) {
    std::lock_guard<std::mutex> lock(mutex_);
    messages_.push_back(std::move(message));
    condition_.notify_all();
  }

  ExceptionOr<ByteArray> Read(std::int64_t size) override {
    std::unique_lock<std::mutex> lock(mutex_);
    condition_.wait(lock, [this]() { return closed_ || !messages_.empty(); });
    if (messages_.empty()) return ExceptionOr<ByteArray>{Exception::kIo};
    std::string result;
    while (!messages_.empty()) {
      absl::StrAppend(&result, messages_.front());
      messages_.pop_front();
      if (size >= 0 && result.size() >= static_cast<size_t>(size)) break;
    }
    if (size >= 0 && result.size() > static_cast<size_t>(size)) {
      std::string remainder = result.substr(size);
      result.resize(size);
      messages_.push_front(std::move(remainder));
    }
    return ExceptionOr<ByteArray>{ByteArray(std::move(result))};
  }

  Exception Close() override {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      closed_ = true;
    }
    condition_.notify_all();
    return {Exception::kSuccess};
  }

 private:
  std::mutex mutex_;
  std::condition_variable condition_;
  std::deque<std::string> messages_;
  bool closed_ = false;
};

class WeaveOutput final : public OutputStream {
 public:
  explicit WeaveOutput(std::shared_ptr<weave::BaseSocket> socket)
      : socket_(std::move(socket)) {}

  Exception Write(absl::string_view data) override {
    if (!socket_) return {Exception::kIo};
    auto future = socket_->Write(ByteArray(std::string(data)));
    auto result = future.Get(absl::Seconds(10));
    if (!result.ok() || !result.result().ok()) return {Exception::kIo};
    return {Exception::kSuccess};
  }

  Exception Flush() override { return {Exception::kSuccess}; }
  Exception Close() override {
    socket_.reset();
    return {Exception::kSuccess};
  }

 private:
  std::shared_ptr<weave::BaseSocket> socket_;
};

class WeaveTransport : public weave::Connection {
 public:
  void Initialize(weave::ConnectionCallback callback) override {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    transmit_cb_ = std::make_shared<absl::AnyInvocable<void(absl::Status)>>(
        std::move(callback.on_transmit_cb));
    receive_cb_ = std::make_shared<absl::AnyInvocable<void(std::string)>>(
        std::move(callback.on_remote_transmit_cb));
    disconnected_cb_ = std::make_shared<absl::AnyInvocable<void()>>(
        std::move(callback.on_disconnected_cb));
  }

  int GetMaxPacketSize() const override { return kWeavePacketSize; }
  void Close() override {
    bool expected = false;
    if (!closed_.compare_exchange_strong(expected, true)) return;
    std::shared_ptr<absl::AnyInvocable<void()>> callback;
    {
      std::lock_guard<std::mutex> lock(callback_mutex_);
      callback = disconnected_cb_;
    }
    if (callback != nullptr && *callback) (*callback)();
  }

  void Receive(const std::string& data) {
    std::shared_ptr<absl::AnyInvocable<void(std::string)>> callback;
    {
      std::lock_guard<std::mutex> lock(callback_mutex_);
      callback = receive_cb_;
    }
    if (callback != nullptr && *callback) (*callback)(data);
  }

 protected:
  void NotifyTransmit(absl::Status status) {
    std::shared_ptr<absl::AnyInvocable<void(absl::Status)>> callback;
    {
      std::lock_guard<std::mutex> lock(callback_mutex_);
      callback = transmit_cb_;
    }
    if (callback != nullptr && *callback) (*callback)(status);
  }

  std::atomic_bool closed_{false};

 private:
  std::mutex callback_mutex_;
  std::shared_ptr<absl::AnyInvocable<void(absl::Status)>> transmit_cb_;
  std::shared_ptr<absl::AnyInvocable<void(std::string)>> receive_cb_;
  std::shared_ptr<absl::AnyInvocable<void()>> disconnected_cb_;
};

class WeaveGattServer {
 public:
  explicit WeaveGattServer(std::function<void(std::string)> on_write)
      : on_write_(std::move(on_write)) {}

  ~WeaveGattServer() { Stop(); }

  bool Start() { return Register(); }

  void Stop() {
    if (!registered_) return;
    registered_ = false;
    if (BluezConnection::Get()->available()) {
      CallMethod(BluezConnection::Get()->connection(),
                 BluezConnection::Get()->adapter_path(), "org.bluez.GattManager1",
                 "UnregisterApplication",
                 g_variant_new("(o)", kApplicationPath));
      for (guint id : registrations_) {
        g_dbus_connection_unregister_object(
            BluezConnection::Get()->connection(), id);
      }
      registrations_.clear();
    }
  }

  bool Notify(const std::string& data) {
    if (!BluezConnection::Get()->available()) return false;
    GVariantBuilder changed;
    g_variant_builder_init(&changed, G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(
        &changed, "{sv}", "Value",
        g_variant_new_fixed_array(G_VARIANT_TYPE_BYTE, data.data(),
                                  data.size(), 1));
    GVariantBuilder invalidated;
    g_variant_builder_init(&invalidated, G_VARIANT_TYPE("as"));
    GError* error = nullptr;
    gboolean result = g_dbus_connection_emit_signal(
        BluezConnection::Get()->connection(), nullptr, kFromPath,
        "org.freedesktop.DBus.Properties", "PropertiesChanged",
        g_variant_new("(s@a{sv}@as)", "org.bluez.GattCharacteristic1",
                      g_variant_builder_end(&changed),
                      g_variant_builder_end(&invalidated)),
        &error);
    if (error != nullptr) g_error_free(error);
    return result;
  }

 private:
  static constexpr char kApplicationPath[] = "/com/tarbum/quickshare/weave";
  static constexpr char kServicePath[] =
      "/com/tarbum/quickshare/weave/service0";
  static constexpr char kToPath[] =
      "/com/tarbum/quickshare/weave/service0/to";
  static constexpr char kFromPath[] =
      "/com/tarbum/quickshare/weave/service0/from";
  static constexpr char kSlotPath[] =
      "/com/tarbum/quickshare/weave/service0/slot0";
  static constexpr char kFromCccdPath[] =
      "/com/tarbum/quickshare/weave/service0/from/desc";

  static void OnMethodCall(GDBusConnection* connection, const gchar* sender,
                           const gchar* object_path,
                           const gchar* interface_name,
                           const gchar* method_name, GVariant* parameters,
                           GDBusMethodInvocation* invocation,
                           gpointer user_data) {
    auto* server = static_cast<WeaveGattServer*>(user_data);
    if (std::strcmp(interface_name,
                    "org.freedesktop.DBus.ObjectManager") == 0 &&
        std::strcmp(method_name, "GetManagedObjects") == 0) {
      g_dbus_method_invocation_return_value(
          invocation,
          g_variant_new("(@a{oa{sa{sv}}})", server->BuildManagedObjects()));
      return;
    }
    if (std::strcmp(interface_name, "org.bluez.GattCharacteristic1") == 0) {
      if (std::strcmp(method_name, "StartNotify") == 0 ||
          std::strcmp(method_name, "StopNotify") == 0) {
        if (std::strcmp(object_path, kFromPath) == 0) {
          server->notifying_ = std::strcmp(method_name, "StartNotify") == 0;
        }
        g_dbus_method_invocation_return_value(invocation, nullptr);
        return;
      }
      if (std::strcmp(method_name, "ReadValue") == 0) {
        std::string value = server->ReadValue(object_path);
        g_dbus_method_invocation_return_value(
            invocation,
            g_variant_new("(@ay)",
                          g_variant_new_fixed_array(G_VARIANT_TYPE_BYTE,
                                                    value.data(), value.size(),
                                                    1)));
        return;
      }
      if (std::strcmp(method_name, "WriteValue") == 0 &&
          std::strcmp(object_path, kToPath) == 0) {
        GVariant* bytes = nullptr;
        GVariant* options = nullptr;
        g_variant_get(parameters, "(@ay@a{sv})", &bytes, &options);
        if (bytes != nullptr) {
          gsize length = 0;
          const auto* data = static_cast<const char*>(
              g_variant_get_fixed_array(bytes, &length, sizeof(char)));
          if (data != nullptr) {
            server->on_write_(std::string(data, length));
          }
          g_variant_unref(bytes);
        }
        if (options != nullptr) g_variant_unref(options);
        g_dbus_method_invocation_return_value(invocation, nullptr);
        return;
      }
    }
    if (std::strcmp(interface_name, "org.bluez.GattDescriptor1") == 0 &&
        std::strcmp(object_path, kFromCccdPath) == 0 &&
        std::strcmp(method_name, "WriteValue") == 0) {
      GVariant* bytes = nullptr;
      GVariant* options = nullptr;
      g_variant_get(parameters, "(@ay@a{sv})", &bytes, &options);
      if (bytes != nullptr) g_variant_unref(bytes);
      if (options != nullptr) g_variant_unref(options);
      g_dbus_method_invocation_return_value(invocation, nullptr);
      return;
    }
    g_dbus_method_invocation_return_dbus_error(
        invocation, "org.bluez.Error.NotSupported", "Unsupported method");
  }

  static GVariant* OnGetProperty(GDBusConnection* connection,
                                 const gchar* sender,
                                 const gchar* object_path,
                                 const gchar* interface_name,
                                 const gchar* property_name, GError** error,
                                 gpointer user_data) {
    return static_cast<WeaveGattServer*>(user_data)->GetProperty(
        object_path, interface_name, property_name);
  }

  bool Register() {
    static const char kApplicationXml[] =
        "<node><interface name='org.freedesktop.DBus.ObjectManager'>"
        "<method name='GetManagedObjects'>"
        "<arg type='a{oa{sa{sv}}}' direction='out'/>"
        "</method></interface></node>";
    static const char kServiceXml[] =
        "<node><interface name='org.bluez.GattService1'>"
        "<property name='UUID' type='s' access='read'/>"
        "<property name='Primary' type='b' access='read'/>"
        "<property name='Characteristics' type='ao' access='read'/>"
        "</interface></node>";
    static const char kCharacteristicXml[] =
        "<node><interface name='org.bluez.GattCharacteristic1'>"
        "<method name='ReadValue'><arg type='a{sv}' direction='in'/>"
        "<arg type='ay' direction='out'/></method>"
        "<method name='WriteValue'><arg type='ay' direction='in'/>"
        "<arg type='a{sv}' direction='in'/></method>"
        "<method name='StartNotify'/>"
        "<method name='StopNotify'/>"
        "<property name='UUID' type='s' access='read'/>"
        "<property name='Service' type='o' access='read'/>"
        "<property name='Flags' type='as' access='read'/>"
        "<property name='Value' type='ay' access='read'/>"
        "<property name='Notifying' type='b' access='read'/>"
        "</interface></node>";
    static const char kDescriptorXml[] =
        "<node><interface name='org.bluez.GattDescriptor1'>"
        "<method name='ReadValue'><arg type='a{sv}' direction='in'/>"
        "<arg type='ay' direction='out'/></method>"
        "<method name='WriteValue'><arg type='ay' direction='in'/>"
        "<arg type='a{sv}' direction='in'/></method>"
        "<property name='UUID' type='s' access='read'/>"
        "<property name='Characteristic' type='o' access='read'/>"
        "<property name='Flags' type='as' access='read'/>"
        "<property name='Value' type='ay' access='read'/>"
        "</interface></node>";

    GDBusNodeInfo* app = g_dbus_node_info_new_for_xml(kApplicationXml, nullptr);
    GDBusNodeInfo* service =
        g_dbus_node_info_new_for_xml(kServiceXml, nullptr);
    GDBusNodeInfo* characteristic =
        g_dbus_node_info_new_for_xml(kCharacteristicXml, nullptr);
    GDBusNodeInfo* descriptor =
        g_dbus_node_info_new_for_xml(kDescriptorXml, nullptr);
    if (app == nullptr || service == nullptr || characteristic == nullptr ||
        descriptor == nullptr) {
      if (app) g_dbus_node_info_unref(app);
      if (service) g_dbus_node_info_unref(service);
      if (characteristic) g_dbus_node_info_unref(characteristic);
      if (descriptor) g_dbus_node_info_unref(descriptor);
      return false;
    }
    static const GDBusInterfaceVTable app_vtable = {
        .method_call = OnMethodCall,
        .get_property = nullptr,
        .set_property = nullptr,
    };
    static const GDBusInterfaceVTable service_vtable = {
        .method_call = nullptr,
        .get_property = OnGetProperty,
        .set_property = nullptr,
    };
    static const GDBusInterfaceVTable characteristic_vtable = {
        .method_call = OnMethodCall,
        .get_property = OnGetProperty,
        .set_property = nullptr,
    };
    GError* error = nullptr;
    guint app_id = g_dbus_connection_register_object(
        BluezConnection::Get()->connection(), kApplicationPath,
        g_dbus_node_info_lookup_interface(app,
                                          "org.freedesktop.DBus.ObjectManager"),
        &app_vtable, this, nullptr, &error);
    guint service_id = g_dbus_connection_register_object(
        BluezConnection::Get()->connection(), kServicePath,
        g_dbus_node_info_lookup_interface(service,
                                          "org.bluez.GattService1"),
        &service_vtable, this, nullptr, &error);
    guint to_id = g_dbus_connection_register_object(
        BluezConnection::Get()->connection(), kToPath,
        g_dbus_node_info_lookup_interface(characteristic,
                                          "org.bluez.GattCharacteristic1"),
        &characteristic_vtable, this, nullptr, &error);
    guint from_id = g_dbus_connection_register_object(
        BluezConnection::Get()->connection(), kFromPath,
        g_dbus_node_info_lookup_interface(characteristic,
                                          "org.bluez.GattCharacteristic1"),
        &characteristic_vtable, this, nullptr, &error);
    guint slot_id = g_dbus_connection_register_object(
        BluezConnection::Get()->connection(), kSlotPath,
        g_dbus_node_info_lookup_interface(characteristic,
                                          "org.bluez.GattCharacteristic1"),
        &characteristic_vtable, this, nullptr, &error);
    guint descriptor_id = g_dbus_connection_register_object(
        BluezConnection::Get()->connection(), kFromCccdPath,
        g_dbus_node_info_lookup_interface(descriptor,
                                          "org.bluez.GattDescriptor1"),
        &characteristic_vtable, this, nullptr, &error);
    g_dbus_node_info_unref(app);
    g_dbus_node_info_unref(service);
    g_dbus_node_info_unref(characteristic);
    g_dbus_node_info_unref(descriptor);
    if (app_id == 0 || service_id == 0 || to_id == 0 || from_id == 0 ||
        slot_id == 0 || descriptor_id == 0) {
      if (error) g_error_free(error);
      return false;
    }
    GVariantBuilder options;
    g_variant_builder_init(&options, G_VARIANT_TYPE("a{sv}"));
    bool ok = CallMethod(
        BluezConnection::Get()->connection(),
        BluezConnection::Get()->adapter_path(), "org.bluez.GattManager1",
        "RegisterApplication",
        g_variant_new("(o@a{sv})", kApplicationPath,
                      g_variant_builder_end(&options)));
    if (!ok) {
      g_dbus_connection_unregister_object(BluezConnection::Get()->connection(),
                                          app_id);
      g_dbus_connection_unregister_object(BluezConnection::Get()->connection(),
                                          service_id);
      g_dbus_connection_unregister_object(BluezConnection::Get()->connection(),
                                          to_id);
      g_dbus_connection_unregister_object(BluezConnection::Get()->connection(),
                                          from_id);
      g_dbus_connection_unregister_object(BluezConnection::Get()->connection(),
                                          slot_id);
      g_dbus_connection_unregister_object(BluezConnection::Get()->connection(),
                                          descriptor_id);
      return false;
    }
    registrations_ = {app_id, service_id, to_id, from_id, slot_id,
                      descriptor_id};
    registered_ = true;
    return true;
  }

  GVariant* BuildManagedObjects() {
    GVariantBuilder objects;
    g_variant_builder_init(&objects, G_VARIANT_TYPE("a{oa{sa{sv}}}"));
    GVariantBuilder service_interfaces;
    g_variant_builder_init(&service_interfaces, G_VARIANT_TYPE("a{sa{sv}}"));
    GVariantBuilder service_props;
    g_variant_builder_init(&service_props, G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(&service_props, "{sv}", "UUID",
                          g_variant_new_string(kWeaveServiceUuid));
    g_variant_builder_add(&service_props, "{sv}", "Primary",
                          g_variant_new_boolean(true));
    GVariantBuilder paths;
    g_variant_builder_init(&paths, G_VARIANT_TYPE("ao"));
    g_variant_builder_add(&paths, "o", kToPath);
    g_variant_builder_add(&paths, "o", kFromPath);
    g_variant_builder_add(&paths, "o", kSlotPath);
    g_variant_builder_add(&service_props, "{sv}", "Characteristics",
                          g_variant_builder_end(&paths));
    g_variant_builder_add(&service_interfaces, "{s@a{sv}}",
                          "org.bluez.GattService1",
                          g_variant_builder_end(&service_props));
    g_variant_builder_add(&objects, "{o@a{sa{sv}}}", kServicePath,
                          g_variant_builder_end(&service_interfaces));

    for (const auto& [path, uuid, flags] :
         std::initializer_list<std::tuple<const char*, const char*,
                                          std::initializer_list<const char*>>>{
             {kToPath, kWeaveToPeripheralUuid, {"write", "write-without-response"}},
             {kFromPath, kWeaveFromPeripheralUuid, {"indicate"}}}) {
      GVariantBuilder interfaces;
      g_variant_builder_init(&interfaces, G_VARIANT_TYPE("a{sa{sv}}"));
      GVariantBuilder props;
      g_variant_builder_init(&props, G_VARIANT_TYPE("a{sv}"));
      g_variant_builder_add(&props, "{sv}", "UUID", g_variant_new_string(uuid));
      g_variant_builder_add(&props, "{sv}", "Service",
                            g_variant_new_object_path(kServicePath));
      GVariantBuilder flag_builder;
      g_variant_builder_init(&flag_builder, G_VARIANT_TYPE("as"));
      for (const char* flag : flags) {
        g_variant_builder_add(&flag_builder, "s", flag);
      }
      g_variant_builder_add(&props, "{sv}", "Flags",
                            g_variant_builder_end(&flag_builder));
      g_variant_builder_add(&props, "{sv}", "Value",
                            g_variant_new_fixed_array(
                                G_VARIANT_TYPE_BYTE, "", 0, 1));
      g_variant_builder_add(&props, "{sv}", "Notifying",
                            g_variant_new_boolean(false));
      if (std::strcmp(path, kFromPath) == 0) {
        GVariantBuilder descriptors;
        g_variant_builder_init(&descriptors, G_VARIANT_TYPE("ao"));
        g_variant_builder_add(&descriptors, "o", kFromCccdPath);
        g_variant_builder_add(&props, "{sv}", "Descriptors",
                              g_variant_builder_end(&descriptors));
      }
      g_variant_builder_add(&interfaces, "{s@a{sv}}",
                            "org.bluez.GattCharacteristic1",
                            g_variant_builder_end(&props));
      g_variant_builder_add(&objects, "{o@a{sa{sv}}}", path,
                            g_variant_builder_end(&interfaces));
    }

    {
      GVariantBuilder interfaces;
      g_variant_builder_init(&interfaces, G_VARIANT_TYPE("a{sa{sv}}"));
      GVariantBuilder props;
      g_variant_builder_init(&props, G_VARIANT_TYPE("a{sv}"));
      g_variant_builder_add(&props, "{sv}", "UUID",
                            g_variant_new_string(kAdvertisementSlotUuid));
      g_variant_builder_add(&props, "{sv}", "Service",
                            g_variant_new_object_path(kServicePath));
      GVariantBuilder flags;
      g_variant_builder_init(&flags, G_VARIANT_TYPE("as"));
      g_variant_builder_add(&flags, "s", "read");
      g_variant_builder_add(&props, "{sv}", "Flags",
                            g_variant_builder_end(&flags));
      std::lock_guard<std::mutex> slot_lock(g_gatt_slot_mutex);
      g_variant_builder_add(
          &props, "{sv}", "Value",
          g_variant_new_fixed_array(G_VARIANT_TYPE_BYTE,
                                    g_gatt_slot_value.data(),
                                    g_gatt_slot_value.size(), 1));
      g_variant_builder_add(&interfaces, "{s@a{sv}}",
                            "org.bluez.GattCharacteristic1",
                            g_variant_builder_end(&props));
      g_variant_builder_add(&objects, "{o@a{sa{sv}}}", kSlotPath,
                            g_variant_builder_end(&interfaces));
    }

    {
      GVariantBuilder interfaces;
      g_variant_builder_init(&interfaces, G_VARIANT_TYPE("a{sa{sv}}"));
      GVariantBuilder props;
      g_variant_builder_init(&props, G_VARIANT_TYPE("a{sv}"));
      g_variant_builder_add(&props, "{sv}", "UUID",
                            g_variant_new_string(kCccdUuid));
      g_variant_builder_add(&props, "{sv}", "Characteristic",
                            g_variant_new_object_path(kFromPath));
      GVariantBuilder flags;
      g_variant_builder_init(&flags, G_VARIANT_TYPE("as"));
      g_variant_builder_add(&flags, "s", "read");
      g_variant_builder_add(&flags, "s", "write");
      g_variant_builder_add(&props, "{sv}", "Flags",
                            g_variant_builder_end(&flags));
      g_variant_builder_add(
          &props, "{sv}", "Value",
          g_variant_new_fixed_array(G_VARIANT_TYPE_BYTE, "\x02\x00", 2, 1));
      g_variant_builder_add(&interfaces, "{s@a{sv}}",
                            "org.bluez.GattDescriptor1",
                            g_variant_builder_end(&props));
      g_variant_builder_add(&objects, "{o@a{sa{sv}}}", kFromCccdPath,
                            g_variant_builder_end(&interfaces));
    }
    return g_variant_builder_end(&objects);
  }

  std::string ReadValue(const gchar* path) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (std::strcmp(path, kToPath) == 0) return to_value_;
    if (std::strcmp(path, kSlotPath) == 0) {
      std::lock_guard<std::mutex> slot_lock(g_gatt_slot_mutex);
      return g_gatt_slot_value;
    }
    if (std::strcmp(path, kFromCccdPath) == 0) return std::string("\x02\x00", 2);
    return from_value_;
  }

  GVariant* GetProperty(const gchar* path, const gchar* interface,
                        const gchar* property) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (std::strcmp(interface, "org.bluez.GattService1") == 0) {
      if (std::strcmp(property, "UUID") == 0) {
        return g_variant_new_string(kWeaveServiceUuid);
      }
      if (std::strcmp(property, "Primary") == 0) {
        return g_variant_new_boolean(true);
      }
      if (std::strcmp(property, "Characteristics") == 0) {
        GVariantBuilder paths;
        g_variant_builder_init(&paths, G_VARIANT_TYPE("ao"));
        g_variant_builder_add(&paths, "o", kToPath);
        g_variant_builder_add(&paths, "o", kFromPath);
        g_variant_builder_add(&paths, "o", kSlotPath);
        return g_variant_builder_end(&paths);
      }
    }
    if (std::strcmp(interface, "org.bluez.GattCharacteristic1") == 0) {
      if (std::strcmp(path, kSlotPath) == 0) {
        if (std::strcmp(property, "UUID") == 0) {
          return g_variant_new_string(kAdvertisementSlotUuid);
        }
        if (std::strcmp(property, "Service") == 0) {
          return g_variant_new_object_path(kServicePath);
        }
        if (std::strcmp(property, "Flags") == 0) {
          GVariantBuilder flags;
          g_variant_builder_init(&flags, G_VARIANT_TYPE("as"));
          g_variant_builder_add(&flags, "s", "read");
          return g_variant_builder_end(&flags);
        }
        if (std::strcmp(property, "Value") == 0) {
          std::lock_guard<std::mutex> slot_lock(g_gatt_slot_mutex);
          return g_variant_new_fixed_array(G_VARIANT_TYPE_BYTE,
                                           g_gatt_slot_value.data(),
                                           g_gatt_slot_value.size(), 1);
        }
        return nullptr;
      }
      bool is_to = std::strcmp(path, kToPath) == 0;
      if (std::strcmp(property, "UUID") == 0) {
        return g_variant_new_string(is_to ? kWeaveToPeripheralUuid
                                         : kWeaveFromPeripheralUuid);
      }
      if (std::strcmp(property, "Service") == 0) {
        return g_variant_new_object_path(kServicePath);
      }
      if (std::strcmp(property, "Flags") == 0) {
        GVariantBuilder flags;
        g_variant_builder_init(&flags, G_VARIANT_TYPE("as"));
        if (is_to) {
          g_variant_builder_add(&flags, "s", "write");
          g_variant_builder_add(&flags, "s", "write-without-response");
        } else {
          g_variant_builder_add(&flags, "s", "indicate");
        }
        return g_variant_builder_end(&flags);
      }
      if (std::strcmp(property, "Value") == 0) {
        const std::string& value = is_to ? to_value_ : from_value_;
        return g_variant_new_fixed_array(G_VARIANT_TYPE_BYTE, value.data(),
                                         value.size(), 1);
      }
      if (std::strcmp(property, "Notifying") == 0) {
        return g_variant_new_boolean(notifying_);
      }
    }
    if (std::strcmp(interface, "org.bluez.GattDescriptor1") == 0 &&
        std::strcmp(path, kFromCccdPath) == 0) {
      if (std::strcmp(property, "UUID") == 0) {
        return g_variant_new_string(kCccdUuid);
      }
      if (std::strcmp(property, "Characteristic") == 0) {
        return g_variant_new_object_path(kFromPath);
      }
      if (std::strcmp(property, "Value") == 0) {
        return g_variant_new_fixed_array(G_VARIANT_TYPE_BYTE, "\x02\x00", 2, 1);
      }
    }
    return nullptr;
  }

  std::function<void(std::string)> on_write_;
  std::mutex mutex_;
  std::string to_value_;
  std::string from_value_;
  bool notifying_ = false;
  bool registered_ = false;
  std::vector<guint> registrations_;
};

class WeaveServerTransport final : public WeaveTransport {
 public:
  void set_gatt(std::shared_ptr<WeaveGattServer> gatt) { gatt_ = std::move(gatt); }

  void Transmit(std::string packet) override {
    if (!first_packet_sent_.exchange(true)) usleep(1500000);
    bool success = gatt_ != nullptr && gatt_->Notify(packet);
    NotifyTransmit(success ? absl::OkStatus()
                           : absl::InternalError("BLE notification failed"));
  }

  std::atomic_bool first_packet_sent_{false};

 private:
  std::shared_ptr<WeaveGattServer> gatt_;
};

class WeaveClientTransport;

class WeaveGattClient {
 public:
  explicit WeaveGattClient(std::string address)
      : address_(std::move(address)) {}
  ~WeaveGattClient() { Close(); }

  bool Connect() {
    LOG(INFO) << "WeaveGattClient connecting to " << address_;
    if (!BluezConnection::Get()->available()) return false;
    if (!FindDevicePath()) {
      LOG(WARNING) << "WeaveGattClient device path not found for " << address_;
      return false;
    }
    bool connected =
        GetBoolProperty(BluezConnection::Get()->connection(), device_path_,
                        "org.bluez.Device1", "Connected")
            .value_or(false);
    if (!connected) {
      for (int attempt = 0; attempt < 3 && !connected; ++attempt) {
        CallMethod(BluezConnection::Get()->connection(), device_path_,
                   "org.bluez.Device1", "Connect", nullptr);
        for (int wait = 0; wait < 10 && !connected; ++wait) {
          usleep(200000);
          connected = GetBoolProperty(BluezConnection::Get()->connection(),
                                      device_path_, "org.bluez.Device1",
                                      "Connected")
                          .value_or(false);
        }
      }
    }
    if (!connected) {
      LOG(WARNING) << "BlueZ LE connect failed for " << address_;
      return false;
    }
    for (int attempt = 0; attempt < 100; ++attempt) {
      auto is_connected = GetBoolProperty(
          BluezConnection::Get()->connection(), device_path_,
          "org.bluez.Device1", "Connected");
      auto services_resolved = GetBoolProperty(
          BluezConnection::Get()->connection(), device_path_,
          "org.bluez.Device1", "ServicesResolved");
      if (is_connected.value_or(false) &&
          services_resolved.value_or(false) && FindCharacteristics()) {
        bool notifying = CallMethod(
            BluezConnection::Get()->connection(), from_path_,
            "org.bluez.GattCharacteristic1", "StartNotify", nullptr);
        if (!notifying) {
          LOG(WARNING) << "BlueZ StartNotify failed for " << address_;
          return false;
        }
        Subscribe();
        return true;
      }
      usleep(100000);
    }
    LOG(WARNING) << "BlueZ GATT characteristics not found for " << address_;
    return false;
  }

  bool Write(const std::string& packet) {
    if (to_path_.empty()) return false;
    GVariantBuilder options;
    g_variant_builder_init(&options, G_VARIANT_TYPE("a{sv}"));
    return CallMethod(
        BluezConnection::Get()->connection(), to_path_,
        "org.bluez.GattCharacteristic1", "WriteValue",
        g_variant_new("(@ay@a{sv})",
                      g_variant_new_fixed_array(G_VARIANT_TYPE_BYTE,
                                                packet.data(), packet.size(),
                                                1),
                      g_variant_builder_end(&options)));
  }

  void Close() {
    if (BluezConnection::Get()->available()) {
      if (subscription_ != 0) {
        g_dbus_connection_signal_unsubscribe(
            BluezConnection::Get()->connection(), subscription_);
        subscription_ = 0;
      }
      if (!device_path_.empty()) {
        CallMethod(BluezConnection::Get()->connection(), device_path_,
                   "org.bluez.Device1", "Disconnect", nullptr);
      }
    }
    to_path_.clear();
    from_path_.clear();
  }

  void set_transport(std::shared_ptr<WeaveClientTransport> transport) {
    transport_ = std::move(transport);
  }

 private:
  static void OnPropertiesChanged(GDBusConnection* connection,
                                  const gchar* sender_name,
                                  const gchar* object_path,
                                  const gchar* interface_name,
                                  const gchar* signal_name,
                                  GVariant* parameters, gpointer user_data) {
    auto* client = static_cast<WeaveGattClient*>(user_data);
    client->HandlePropertiesChanged(object_path, parameters);
  }

  void HandlePropertiesChanged(const gchar* object_path,
                               GVariant* parameters);

  void Subscribe() {
    subscription_ = g_dbus_connection_signal_subscribe(
        BluezConnection::Get()->connection(), nullptr,
        "org.freedesktop.DBus.Properties", "PropertiesChanged", nullptr, nullptr,
        G_DBUS_SIGNAL_FLAGS_NONE, OnPropertiesChanged, this, nullptr);
  }

  bool FindDevicePath() {
    GError* error = nullptr;
    GVariant* reply = g_dbus_connection_call_sync(
        BluezConnection::Get()->connection(), kBlueZService, "/",
        "org.freedesktop.DBus.ObjectManager", "GetManagedObjects", nullptr,
        G_VARIANT_TYPE("(a{oa{sa{sv}}})"), G_DBUS_CALL_FLAGS_NONE,
        kDBusTimeoutMs, nullptr, &error);
    if (reply == nullptr) {
      if (error) g_error_free(error);
      return false;
    }
    GVariant* objects = nullptr;
    g_variant_get(reply, "(@a{oa{sa{sv}}})", &objects);
    GVariantIter iter;
    g_variant_iter_init(&iter, objects);
    const gchar* path = nullptr;
    GVariant* interfaces = nullptr;
    std::string named_weave_path;
    int best_named_rssi = -1000;
    while (g_variant_iter_next(&iter, "{&o@a{sa{sv}}}", &path, &interfaces)) {
      GVariant* props = g_variant_lookup_value(
          interfaces, "org.bluez.Device1", G_VARIANT_TYPE("a{sv}"));
      if (props != nullptr) {
        GVariant* address = g_variant_lookup_value(
            props, "Address", G_VARIANT_TYPE_STRING);
        if (address != nullptr &&
            Lowercase(VariantToString(address)) == Lowercase(address_)) {
          device_path_ = path;
        }
        if (address != nullptr) g_variant_unref(address);

        bool descriptive_name = false;
        GVariant* name = g_variant_lookup_value(
            props, "Name", G_VARIANT_TYPE_STRING);
        if (name != nullptr) {
          std::string value = VariantToString(name);
          descriptive_name = !value.empty() && value.rfind("QS", 0) != 0;
          g_variant_unref(name);
        }
        if (!descriptive_name) {
          GVariant* alias = g_variant_lookup_value(
              props, "Alias", G_VARIANT_TYPE_STRING);
          if (alias != nullptr) {
            std::string value = VariantToString(alias);
            bool mac_like = value.size() == 17;
            for (char c : value) {
              if (c != '-' && !std::isxdigit(static_cast<unsigned char>(c))) {
                mac_like = false;
                break;
              }
            }
            descriptive_name = !value.empty() && !mac_like &&
                               value.rfind("QS", 0) != 0;
            g_variant_unref(alias);
          }
        }
        bool has_weave = false;
        GVariant* uuids = g_variant_lookup_value(
            props, "UUIDs", G_VARIANT_TYPE("as"));
        if (uuids != nullptr) {
          GVariantIter uuid_iter;
          const gchar* uuid = nullptr;
          g_variant_iter_init(&uuid_iter, uuids);
          while (g_variant_iter_next(&uuid_iter, "&s", &uuid)) {
            if (Lowercase(uuid) == Lowercase(kWeaveServiceUuid)) {
              has_weave = true;
              break;
            }
          }
          g_variant_unref(uuids);
        }
        if (!has_weave) {
          GVariant* service_data = g_variant_lookup_value(
              props, "ServiceData", G_VARIANT_TYPE("a{sv}"));
          if (service_data != nullptr) {
            GVariantIter data_iter;
            const gchar* data_uuid = nullptr;
            GVariant* data_value = nullptr;
            g_variant_iter_init(&data_iter, service_data);
            while (g_variant_iter_next(&data_iter, "{&s@v}", &data_uuid,
                                       &data_value)) {
              if (Lowercase(data_uuid) == Lowercase(kWeaveServiceUuid)) {
                has_weave = true;
              }
              g_variant_unref(data_value);
              if (has_weave) break;
            }
            g_variant_unref(service_data);
          }
        }
        int rssi = -1000;
        GVariant* rssi_value = g_variant_lookup_value(
            props, "RSSI", G_VARIANT_TYPE_INT16);
        if (rssi_value != nullptr) {
          rssi = g_variant_get_int16(rssi_value);
          g_variant_unref(rssi_value);
        }
        if (descriptive_name && has_weave && rssi > best_named_rssi) {
          best_named_rssi = rssi;
          named_weave_path = path;
        }
        g_variant_unref(props);
      }
      g_variant_unref(interfaces);
    }
    g_variant_unref(objects);
    g_variant_unref(reply);

    if (!device_path_.empty()) {
      LOG(INFO) << "Using BlueZ peer " << device_path_ << " for " << address_;
    }
    return !device_path_.empty();
  }

  bool FindCharacteristics() {
    GError* error = nullptr;
    GVariant* reply = g_dbus_connection_call_sync(
        BluezConnection::Get()->connection(), kBlueZService, "/",
        "org.freedesktop.DBus.ObjectManager", "GetManagedObjects", nullptr,
        G_VARIANT_TYPE("(a{oa{sa{sv}}})"), G_DBUS_CALL_FLAGS_NONE,
        kDBusTimeoutMs, nullptr, &error);
    if (reply == nullptr) {
      if (error) g_error_free(error);
      return false;
    }
    GVariant* objects = nullptr;
    g_variant_get(reply, "(@a{oa{sa{sv}}})", &objects);
    GVariantIter iter;
    const gchar* path = nullptr;
    GVariant* interfaces = nullptr;
    std::vector<std::string> services;
    g_variant_iter_init(&iter, objects);
    while (g_variant_iter_next(&iter, "{&o@a{sa{sv}}}", &path, &interfaces)) {
      GVariant* props = g_variant_lookup_value(
          interfaces, "org.bluez.GattService1", G_VARIANT_TYPE("a{sv}"));
      if (props != nullptr) {
        GVariant* uuid = g_variant_lookup_value(
            props, "UUID", G_VARIANT_TYPE_STRING);
        bool matching_device =
            path != nullptr &&
            std::string(path).rfind(device_path_ + "/", 0) == 0;
        if (uuid != nullptr && matching_device &&
            Lowercase(VariantToString(uuid)) == Lowercase(kWeaveServiceUuid)) {
          services.push_back(path);
        }
        if (uuid != nullptr) g_variant_unref(uuid);
        g_variant_unref(props);
      }
      g_variant_unref(interfaces);
    }
    for (const auto& service_path : services) {
      std::string to_path;
      std::string from_path;
      g_variant_iter_init(&iter, objects);
      while (g_variant_iter_next(&iter, "{&o@a{sa{sv}}}", &path,
                                 &interfaces)) {
        GVariant* props = g_variant_lookup_value(
            interfaces, "org.bluez.GattCharacteristic1",
            G_VARIANT_TYPE("a{sv}"));
        if (props != nullptr) {
          GVariant* uuid = g_variant_lookup_value(
              props, "UUID", G_VARIANT_TYPE_STRING);
          GVariant* service = g_variant_lookup_value(
              props, "Service", G_VARIANT_TYPE_OBJECT_PATH);
          if (uuid != nullptr && service != nullptr &&
              VariantToString(service) == service_path) {
            std::string value = Lowercase(VariantToString(uuid));
            if (value == Lowercase(kWeaveToPeripheralUuid)) to_path = path;
            if (value == Lowercase(kWeaveFromPeripheralUuid)) from_path = path;
          }
          if (uuid != nullptr) g_variant_unref(uuid);
          if (service != nullptr) g_variant_unref(service);
          g_variant_unref(props);
        }
        g_variant_unref(interfaces);
      }
      if (!to_path.empty() && !from_path.empty()) {
        to_path_ = to_path;
        from_path_ = from_path;
        g_variant_unref(objects);
        g_variant_unref(reply);
        return true;
      }
    }
    g_variant_unref(objects);
    g_variant_unref(reply);
    return false;
  }

  std::string address_;
  std::string device_path_;
  std::string to_path_;
  std::string from_path_;
  guint subscription_ = 0;
  std::shared_ptr<WeaveClientTransport> transport_;
};

class WeaveClientTransport final : public WeaveTransport {
 public:
  explicit WeaveClientTransport(std::shared_ptr<WeaveGattClient> client)
      : client_(std::move(client)) {}

  void Transmit(std::string packet) override {
    bool success = client_ != nullptr && client_->Write(packet);
    NotifyTransmit(success ? absl::OkStatus()
                           : absl::InternalError("BLE write failed"));
  }

 private:
  std::shared_ptr<WeaveGattClient> client_;
};

void WeaveGattClient::HandlePropertiesChanged(const gchar* object_path,
                                             GVariant* parameters) {
  if (from_path_.empty() || std::strcmp(object_path, from_path_.c_str()) != 0) {
    return;
  }
  const gchar* interface = nullptr;
  GVariant* changed = nullptr;
  GVariant* invalidated = nullptr;
  g_variant_get(parameters, "(&s@a{sv}@as)", &interface, &changed, &invalidated);
  if (std::strcmp(interface, "org.bluez.GattCharacteristic1") == 0) {
    GVariant* value =
        g_variant_lookup_value(changed, "Value", G_VARIANT_TYPE("ay"));
    if (value != nullptr) {
      gsize length = 0;
      const auto* data = static_cast<const char*>(
          g_variant_get_fixed_array(value, &length, sizeof(char)));
      if (data != nullptr && transport_ != nullptr) {
        transport_->Receive(std::string(data, length));
      }
      g_variant_unref(value);
    }
  }
  g_variant_unref(changed);
  g_variant_unref(invalidated);
}

class WeaveBleSocketImpl final : public api::ble::BleSocket {
 public:
  WeaveBleSocketImpl(std::shared_ptr<WeaveTransport> transport,
                     std::shared_ptr<weave::BaseSocket> socket,
                     std::shared_ptr<WeaveInput> input,
                     std::shared_ptr<WeaveOutput> output,
                     BlePeripheral::UniqueId peripheral_id)
      : transport_(std::move(transport)),
        socket_(std::move(socket)),
        input_(std::move(input)),
        output_(std::move(output)),
        peripheral_id_(peripheral_id) {}

  InputStream& GetInputStream() override { return *input_; }
  OutputStream& GetOutputStream() override { return *output_; }
  Exception Close() override {
    bool expected = false;
    if (!closed_.compare_exchange_strong(expected, true)) {
      return {Exception::kSuccess};
    }
    input_->Close();
    output_->Close();
    if (socket_ != nullptr) socket_->Disconnect();
    if (transport_ != nullptr) transport_->Close();
    return {Exception::kSuccess};
  }
  BlePeripheral::UniqueId GetRemotePeripheralId() override {
    return peripheral_id_;
  }

 private:
  std::shared_ptr<WeaveTransport> transport_;
  std::shared_ptr<weave::BaseSocket> socket_;
  std::shared_ptr<WeaveInput> input_;
  std::shared_ptr<WeaveOutput> output_;
  BlePeripheral::UniqueId peripheral_id_;
  std::atomic_bool closed_{false};
};

class WeaveServerSocketImpl final : public api::ble::BleServerSocket {
 public:
  WeaveServerSocketImpl() {
    input_ = std::make_shared<WeaveInput>();
    transport_ = std::make_shared<WeaveServerTransport>();
    gatt_ = std::make_shared<WeaveGattServer>(
        [this](std::string data) { transport_->Receive(data); });
    transport_->set_gatt(gatt_);
    auto state = this;
    socket_ = std::make_shared<weave::ServerSocket>(
        *transport_,
        weave::SocketCallback{
            .on_connected_cb = [state]() { state->OnConnected(); },
            .on_disconnected_cb = [state]() { state->OnDisconnected(); },
            .on_receive_cb =
                [input = input_](std::string message) {
                  input->Push(std::move(message));
                },
            .on_error_cb = [](absl::Status status) {
              LOG(WARNING) << "BLE weave server error: " << status;
            },
        });
    if (!gatt_->Start()) {
      closed_ = true;
    }
  }

  ~WeaveServerSocketImpl() override { Close(); }

  std::unique_ptr<api::ble::BleSocket> Accept() override {
    std::unique_lock<std::mutex> lock(mutex_);
    condition_.wait(lock, [this]() {
      return closed_ || !pending_.empty();
    });
    if (pending_.empty()) return nullptr;
    auto socket = std::move(pending_.front());
    pending_.pop_front();
    return socket;
  }

  Exception Close() override {
    bool expected = false;
    if (!closed_.compare_exchange_strong(expected, true)) {
      return {Exception::kSuccess};
    }
    input_->Close();
    if (transport_ != nullptr) transport_->Close();
    condition_.notify_all();
    return {Exception::kSuccess};
  }

 private:
  void OnConnected() {
    auto output = std::make_shared<WeaveOutput>(socket_);
    auto socket = std::make_unique<WeaveBleSocketImpl>(
        transport_, socket_, input_, output, 0);
    {
      std::lock_guard<std::mutex> lock(mutex_);
      pending_.push_back(std::move(socket));
    }
    condition_.notify_all();
  }

  void OnDisconnected() {
    input_->Close();
    condition_.notify_all();
  }

  std::shared_ptr<WeaveInput> input_;
  std::shared_ptr<WeaveServerTransport> transport_;
  std::shared_ptr<WeaveGattServer> gatt_;
  std::shared_ptr<weave::ServerSocket> socket_;
  std::mutex mutex_;
  std::condition_variable condition_;
  std::deque<std::unique_ptr<api::ble::BleSocket>> pending_;
  std::atomic_bool closed_{false};
};

}  // namespace


namespace {

struct WeaveHandshakeState {
  std::mutex mutex;
  std::condition_variable condition;
  bool connected = false;
  bool failed = false;
};

}  // namespace

std::unique_ptr<api::ble::BleServerSocket>
BleMediumImpl::OpenServerSocket(const std::string& service_id) {
  return std::make_unique<WeaveServerSocketImpl>();
}

std::unique_ptr<api::ble::BleSocket> BleMediumImpl::Connect(
    const std::string& service_id, TxPowerLevel tx_power_level,
    BlePeripheral::UniqueId peripheral_id,
    CancellationFlag* cancellation_flag) {
  if (cancellation_flag != nullptr && cancellation_flag->Cancelled()) {
    return nullptr;
  }
  std::string address;
  {
    std::lock_guard<std::mutex> lock(peripherals_mutex_);
    auto it = id_to_address_.find(peripheral_id);
    if (it == id_to_address_.end()) {
      LOG(WARNING) << "No BLE address for peripheral id " << peripheral_id;
      return nullptr;
    }
    address = it->second;
  }
  LOG(INFO) << "BLE connect peripheral id " << peripheral_id << " address "
            << address;
  CallMethod(BluezConnection::Get()->connection(),
             BluezConnection::Get()->adapter_path(), kAdapterInterface,
             "StopDiscovery", nullptr);
  usleep(300000);
  auto client = std::make_shared<WeaveGattClient>(address);
  bool connected = client->Connect();
  CallMethod(BluezConnection::Get()->connection(),
             BluezConnection::Get()->adapter_path(), kAdapterInterface,
             "StartDiscovery", nullptr);
  if (!connected) return nullptr;
  auto transport = std::make_shared<WeaveClientTransport>(client);
  client->set_transport(transport);
  auto input = std::make_shared<WeaveInput>();
  auto state = std::make_shared<WeaveHandshakeState>();
  auto weave_socket = std::make_shared<weave::ClientSocket>(
      *transport,
      weave::SocketCallback{
          .on_connected_cb =
              [state]() {
                std::lock_guard<std::mutex> lock(state->mutex);
                state->connected = true;
                state->condition.notify_all();
              },
          .on_disconnected_cb =
              [state]() {
                std::lock_guard<std::mutex> lock(state->mutex);
                state->failed = true;
                state->condition.notify_all();
              },
          .on_receive_cb =
              [input](std::string message) { input->Push(std::move(message)); },
          .on_error_cb =
              [state](absl::Status status) {
                LOG(WARNING) << "BLE weave client error: " << status;
                std::lock_guard<std::mutex> lock(state->mutex);
                state->failed = true;
                state->condition.notify_all();
              },
      });
  auto output = std::make_shared<WeaveOutput>(weave_socket);
  auto socket = std::make_unique<WeaveBleSocketImpl>(
      transport, weave_socket, input, output, peripheral_id);
  weave_socket->Connect();
  std::unique_lock<std::mutex> lock(state->mutex);
  state->condition.wait_for(lock, std::chrono::seconds(5),
                            [state]() {
                              return state->connected || state->failed;
                            });
  if (!state->connected) {
    socket->Close();
    return nullptr;
  }
  return socket;
}

}  // namespace linux_platform
}  // namespace nearby

namespace nearby {
namespace linux_platform {

class AdvertisementGattClient final : public GattClient {
 public:
  explicit AdvertisementGattClient(std::string address)
      : address_(std::move(address)) {}
  ~AdvertisementGattClient() override { Disconnect(); }

  bool DiscoverServiceAndCharacteristics(
      const Uuid& service_uuid,
      const std::vector<Uuid>& characteristic_uuids) override {
    if (!FindDevicePath()) return false;
    if (!connected_) {
      if (!CallMethod(BluezConnection::Get()->connection(), device_path_,
                      "org.bluez.Device1", "Connect", nullptr)) {
        return false;
      }
      connected_ = true;
    }
    for (int attempt = 0; attempt < 50; ++attempt) {
      if (FindCharacteristics(service_uuid)) return true;
      usleep(100000);
    }
    return false;
  }

  std::optional<GattCharacteristic> GetCharacteristic(
      const Uuid& service_uuid, const Uuid& characteristic_uuid) override {
    auto it = characteristics_.find(std::string(characteristic_uuid));
    if (it == characteristics_.end()) return std::nullopt;
    return GattCharacteristic{
        .uuid = characteristic_uuid,
        .service_uuid = service_uuid,
        .permission = GattCharacteristic::Permission::kRead,
        .property = GattCharacteristic::Property::kRead,
    };
  }

  std::optional<std::string> ReadCharacteristic(
      const GattCharacteristic& characteristic) override {
    auto it = characteristics_.find(std::string(characteristic.uuid));
    if (it == characteristics_.end()) return std::nullopt;
    GVariantBuilder options;
    g_variant_builder_init(&options, G_VARIANT_TYPE("a{sv}"));
    GError* error = nullptr;
    GVariant* reply = g_dbus_connection_call_sync(
        BluezConnection::Get()->connection(), kBlueZService, it->second.c_str(),
        "org.bluez.GattCharacteristic1", "ReadValue",
        g_variant_new("(@a{sv})", g_variant_builder_end(&options)),
        G_VARIANT_TYPE("(ay)"), G_DBUS_CALL_FLAGS_NONE, kDBusTimeoutMs,
        nullptr, &error);
    if (reply == nullptr) {
      if (error) g_error_free(error);
      return std::nullopt;
    }
    GVariant* bytes = nullptr;
    g_variant_get(reply, "(@ay)", &bytes);
    std::optional<std::string> result;
    if (bytes != nullptr && g_variant_is_of_type(bytes, G_VARIANT_TYPE("ay"))) {
      gsize length = 0;
      const auto* data = static_cast<const char*>(
          g_variant_get_fixed_array(bytes, &length, sizeof(char)));
      if (data != nullptr) result = std::string(data, length);
    }
    if (bytes != nullptr) g_variant_unref(bytes);
    g_variant_unref(reply);
    return result;
  }

  bool WriteCharacteristic(const GattCharacteristic& characteristic,
                           absl::string_view value, WriteType type) override {
    auto it = characteristics_.find(std::string(characteristic.uuid));
    if (it == characteristics_.end()) return false;
    GVariantBuilder options;
    g_variant_builder_init(&options, G_VARIANT_TYPE("a{sv}"));
    return CallMethod(
        BluezConnection::Get()->connection(), it->second.c_str(),
        "org.bluez.GattCharacteristic1", "WriteValue",
        g_variant_new("(@ay@a{sv})",
                      g_variant_new_fixed_array(G_VARIANT_TYPE_BYTE,
                                                value.data(), value.size(), 1),
                      g_variant_builder_end(&options)));
  }

  void Disconnect() override {
    if (connected_ && BluezConnection::Get()->available() &&
        !device_path_.empty()) {
      CallMethod(BluezConnection::Get()->connection(), device_path_,
                 "org.bluez.Device1", "Disconnect", nullptr);
    }
    connected_ = false;
    characteristics_.clear();
  }

 private:
  bool FindDevicePath() {
    if (!BluezConnection::Get()->available()) return false;
    GError* error = nullptr;
    GVariant* reply = g_dbus_connection_call_sync(
        BluezConnection::Get()->connection(), kBlueZService, "/",
        "org.freedesktop.DBus.ObjectManager", "GetManagedObjects", nullptr,
        G_VARIANT_TYPE("(a{oa{sa{sv}}})"), G_DBUS_CALL_FLAGS_NONE,
        kDBusTimeoutMs, nullptr, &error);
    if (reply == nullptr) {
      if (error) g_error_free(error);
      return false;
    }
    GVariant* objects = nullptr;
    g_variant_get(reply, "(@a{oa{sa{sv}}})", &objects);
    GVariantIter iter;
    g_variant_iter_init(&iter, objects);
    const gchar* path = nullptr;
    GVariant* interfaces = nullptr;
    while (g_variant_iter_next(&iter, "{&o@a{sa{sv}}}", &path, &interfaces)) {
      GVariant* props = g_variant_lookup_value(
          interfaces, "org.bluez.Device1", G_VARIANT_TYPE("a{sv}"));
      if (props != nullptr) {
        GVariant* address = g_variant_lookup_value(
            props, "Address", G_VARIANT_TYPE_STRING);
        if (address != nullptr &&
            Lowercase(VariantToString(address)) == Lowercase(address_)) {
          device_path_ = path;
          g_variant_unref(address);
          g_variant_unref(props);
          g_variant_unref(interfaces);
          break;
        }
        if (address != nullptr) g_variant_unref(address);
        g_variant_unref(props);
      }
      g_variant_unref(interfaces);
    }
    g_variant_unref(objects);
    g_variant_unref(reply);
    return !device_path_.empty();
  }

  bool FindCharacteristics(const Uuid& service_uuid) {
    GError* error = nullptr;
    GVariant* reply = g_dbus_connection_call_sync(
        BluezConnection::Get()->connection(), kBlueZService, "/",
        "org.freedesktop.DBus.ObjectManager", "GetManagedObjects", nullptr,
        G_VARIANT_TYPE("(a{oa{sa{sv}}})"), G_DBUS_CALL_FLAGS_NONE,
        kDBusTimeoutMs, nullptr, &error);
    if (reply == nullptr) {
      if (error) g_error_free(error);
      return false;
    }
    GVariant* objects = nullptr;
    g_variant_get(reply, "(@a{oa{sa{sv}}})", &objects);
    GVariantIter iter;
    g_variant_iter_init(&iter, objects);
    const gchar* path = nullptr;
    GVariant* interfaces = nullptr;
    std::string service_path;
    std::string desired_service = Lowercase(std::string(service_uuid));
    while (g_variant_iter_next(&iter, "{&o@a{sa{sv}}}", &path, &interfaces)) {
      GVariant* props = g_variant_lookup_value(
          interfaces, "org.bluez.GattService1", G_VARIANT_TYPE("a{sv}"));
      if (props != nullptr) {
        GVariant* uuid =
            g_variant_lookup_value(props, "UUID", G_VARIANT_TYPE_STRING);
        if (uuid != nullptr &&
            Lowercase(VariantToString(uuid)) == desired_service) {
          service_path = path;
          g_variant_unref(uuid);
          g_variant_unref(props);
          g_variant_unref(interfaces);
          break;
        }
        if (uuid != nullptr) g_variant_unref(uuid);
        g_variant_unref(props);
      }
      g_variant_unref(interfaces);
    }
    if (service_path.empty()) {
      g_variant_unref(objects);
      g_variant_unref(reply);
      return false;
    }
    g_variant_iter_init(&iter, objects);
    while (g_variant_iter_next(&iter, "{&o@a{sa{sv}}}", &path, &interfaces)) {
      GVariant* props = g_variant_lookup_value(
          interfaces, "org.bluez.GattCharacteristic1", G_VARIANT_TYPE("a{sv}"));
      if (props != nullptr) {
        GVariant* uuid =
            g_variant_lookup_value(props, "UUID", G_VARIANT_TYPE_STRING);
        GVariant* service = g_variant_lookup_value(
            props, "Service", G_VARIANT_TYPE_OBJECT_PATH);
        if (uuid != nullptr && service != nullptr &&
            VariantToString(service) == service_path) {
          characteristics_[std::string(
              VariantToString(uuid))] = std::string(path);
        }
        if (uuid != nullptr) g_variant_unref(uuid);
        if (service != nullptr) g_variant_unref(service);
        g_variant_unref(props);
      }
      g_variant_unref(interfaces);
    }
    g_variant_unref(objects);
    g_variant_unref(reply);
    return !characteristics_.empty();
  }

  std::string address_;
  std::string device_path_;
  bool connected_ = false;
  std::unordered_map<std::string, std::string> characteristics_;
};

std::unique_ptr<GattClient> BleMediumImpl::ConnectToGattServer(
    BlePeripheral::UniqueId peripheral_id, TxPowerLevel tx_power_level,
    ClientGattConnectionCallback callback) {
  std::string address;
  {
    std::lock_guard<std::mutex> lock(peripherals_mutex_);
    auto it = id_to_address_.find(peripheral_id);
    if (it == id_to_address_.end()) return nullptr;
    address = it->second;
  }
  return std::make_unique<AdvertisementGattClient>(address);
}

class L2capInputStream final : public InputStream {
 public:
  explicit L2capInputStream(int fd) : fd_(fd) {}
  ExceptionOr<ByteArray> Read(std::int64_t size) override {
    std::lock_guard<std::mutex> lock(mutex_);
    if (buffer_.empty()) {
      char temp[8192];
      ssize_t received = recv(fd_, temp, sizeof(temp), 0);
      if (received <= 0) return ExceptionOr<ByteArray>{Exception::kIo};
      buffer_.assign(temp, static_cast<size_t>(received));
    }
    size_t count = size < 0 ? buffer_.size() : std::min<size_t>(size, buffer_.size());
    std::string result = buffer_.substr(0, count);
    buffer_.erase(0, count);
    return ExceptionOr<ByteArray>{ByteArray(std::move(result))};
  }
  Exception Close() override { return {Exception::kSuccess}; }
 private:
  int fd_;
  std::mutex mutex_;
  std::string buffer_;
};

class L2capOutputStream final : public OutputStream {
 public:
  explicit L2capOutputStream(int fd) : fd_(fd) {}
  Exception Write(absl::string_view data) override {
    size_t offset = 0;
    while (offset < data.size()) {
      size_t chunk = std::min<size_t>(data.size() - offset, 16384);
      ssize_t sent = send(fd_, data.data() + offset, chunk, 0);
      if (sent <= 0) return {Exception::kIo};
      offset += static_cast<size_t>(sent);
    }
    return {Exception::kSuccess};
  }
  Exception Flush() override { return {Exception::kSuccess}; }
  Exception Close() override { return {Exception::kSuccess}; }
 private:
  int fd_;
};

class L2capSocketImpl final : public BleL2capSocket {
 public:
  L2capSocketImpl(int fd, BlePeripheral::UniqueId peripheral_id)
      : fd_(fd), input_(fd), output_(fd), peripheral_id_(peripheral_id) {}
  ~L2capSocketImpl() override { Close(); }
  InputStream& GetInputStream() override { return input_; }
  OutputStream& GetOutputStream() override { return output_; }
  Exception Close() override {
    bool expected = false;
    if (!closed_.compare_exchange_strong(expected, true)) {
      return {Exception::kSuccess};
    }
    shutdown(fd_, SHUT_RDWR);
    close(fd_);
    if (notifier_) {
      auto notifier = std::move(notifier_);
      notifier();
    }
    return {Exception::kSuccess};
  }
  void SetCloseNotifier(absl::AnyInvocable<void()> notifier) override {
    notifier_ = std::move(notifier);
  }
  BlePeripheral::UniqueId GetRemotePeripheralId() override {
    return peripheral_id_;
  }
 private:
  int fd_;
  L2capInputStream input_;
  L2capOutputStream output_;
  BlePeripheral::UniqueId peripheral_id_;
  std::atomic_bool closed_{false};
  absl::AnyInvocable<void()> notifier_;
};

std::unique_ptr<BleL2capSocket> BleMediumImpl::ConnectOverL2cap(
    int psm, const std::string& service_id, TxPowerLevel tx_power_level,
    BlePeripheral::UniqueId peripheral_id,
    CancellationFlag* cancellation_flag) {
  if (std::getenv("QUICKSHARE_ENABLE_BLE_L2CAP") == nullptr) {
    return nullptr;
  }
  if (cancellation_flag != nullptr && cancellation_flag->Cancelled()) {
    return nullptr;
  }
  std::string address;
  uint8_t address_type = 0;
  {
    std::lock_guard<std::mutex> lock(peripherals_mutex_);
    auto it = id_to_address_.find(peripheral_id);
    if (it == id_to_address_.end()) return nullptr;
    address = it->second;
    auto type_it = id_to_address_type_.find(peripheral_id);
    if (type_it != id_to_address_type_.end()) address_type = type_it->second;
  }
  unsigned int values[6];
  if (sscanf(address.c_str(), "%x:%x:%x:%x:%x:%x", &values[0], &values[1],
             &values[2], &values[3], &values[4], &values[5]) != 6) {
    return nullptr;
  }
  LOG(INFO) << "L2CAP connecting to " << address << " psm " << psm
            << " type " << static_cast<int>(address_type);
  for (int attempt_psm : {psm, psm + 1}) {
    int fd = socket(AF_BLUETOOTH, SOCK_SEQPACKET, BTPROTO_L2CAP);
    if (fd < 0) return nullptr;
    sockaddr_l2 remote{};
    remote.l2_family = AF_BLUETOOTH;
    remote.l2_psm = htobs(attempt_psm);
    remote.l2_bdaddr_type = address_type;
    for (int i = 0; i < 6; ++i) {
      remote.l2_bdaddr.b[i] = static_cast<uint8_t>(values[5 - i]);
    }
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    int result =
        connect(fd, reinterpret_cast<sockaddr*>(&remote), sizeof(remote));
    if (result < 0 && errno == EINPROGRESS) {
      pollfd pfd{fd, POLLOUT, 0};
      result = poll(&pfd, 1, 3000);
      if (result > 0) {
        int error = 0;
        socklen_t length = sizeof(error);
        getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &length);
        result = error == 0 ? 0 : -1;
        errno = error;
      } else {
        result = -1;
        errno = ETIMEDOUT;
      }
    }
    fcntl(fd, F_SETFL, flags);
    if (result == 0) {
      LOG(INFO) << "L2CAP connected to " << address << " psm " << attempt_psm;
      return std::make_unique<L2capSocketImpl>(fd, peripheral_id);
    }
    LOG(WARNING) << "L2CAP connect psm " << attempt_psm << " failed: "
                 << strerror(errno);
    close(fd);
  }
  return nullptr;
}

}  // namespace linux_platform
}  // namespace nearby
