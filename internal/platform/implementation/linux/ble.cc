#include "internal/platform/implementation/linux/ble.h"

#include <gio/gio.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
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
#include "absl/strings/str_cat.h"
#include "absl/strings/string_view.h"
#include "internal/platform/byte_array.h"
#include "internal/platform/cancellation_flag.h"
#include "internal/platform/exception.h"
#include "internal/platform/implementation/ble.h"
#include "internal/platform/logging.h"
#include "internal/platform/mac_address.h"
#include "internal/platform/uuid.h"

namespace nearby {
namespace linux_platform {
namespace {

using ::nearby::api::ble::AdvertiseParameters;
using ::nearby::api::ble::BleAdvertisementData;
using ::nearby::api::ble::BlePeripheral;
using ::nearby::api::ble::ClientGattConnectionCallback;
using ::nearby::api::ble::GattCharacteristic;
using ::nearby::api::ble::GattClient;
using ::nearby::api::ble::GattServer;
using ::nearby::api::ble::ServerGattConnectionCallback;
using ::nearby::api::ble::TxPowerLevel;

constexpr char kBlueZService[] = "org.bluez";
constexpr char kAdapterInterface[] = "org.bluez.Adapter1";
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
        "org.freedesktop.DBus.Properties", "PropertiesChanged", "/", nullptr,
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
      ClientGattConnectionCallback callback) override {
    return nullptr;
  }

  std::unique_ptr<api::ble::BleServerSocket> OpenServerSocket(
      const std::string& service_id) override {
    return std::make_unique<DummyBleServerSocket>();
  }

  std::unique_ptr<api::ble::BleSocket> Connect(
      const std::string& service_id, TxPowerLevel tx_power_level,
      BlePeripheral::UniqueId peripheral_id,
      CancellationFlag* cancellation_flag) override {
    return nullptr;
  }

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
    if (!service_uuids.empty()) {
      GVariantBuilder uuids;
      g_variant_builder_init(&uuids, G_VARIANT_TYPE("as"));
      for (const auto& uuid : service_uuids) {
        std::string text = static_cast<std::string>(uuid);
        g_variant_builder_add(&uuids, "s", text.c_str());
      }
      g_variant_builder_add(&filter, "{sv}", "UUIDs",
                            g_variant_builder_end(&uuids));
    }
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
    g_variant_get(parameters, "(&o@a{sv})", &path, &interfaces);
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
    if (service_data == nullptr) return;

    BleAdvertisementData advertisement;
    advertisement.is_extended_advertisement = false;
    GVariantIter iter;
    g_variant_iter_init(&iter, service_data);
    const gchar* uuid_text = nullptr;
    GVariant* value = nullptr;
    while (g_variant_iter_next(&iter, "{&s@v}", &uuid_text, &value)) {
      GVariant* bytes = g_variant_get_variant(value);
      if (bytes != nullptr &&
          g_variant_is_of_type(bytes, G_VARIANT_TYPE("ay"))) {
        gsize length = 0;
        const auto* data =
            static_cast<const char*>(g_variant_get_fixed_array(
                bytes, &length, sizeof(char)));
        auto uuid = Uuid::FromString(uuid_text);
        if (uuid.has_value() && data != nullptr && length > 0) {
          advertisement.service_data.emplace(*uuid,
                                             ByteArray(data, length));
        }
      }
      if (bytes != nullptr) g_variant_unref(bytes);
      g_variant_unref(value);
    }
    g_variant_unref(service_data);

    if (advertisement.service_data.empty()) return;

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

    std::shared_ptr<ScanCallback> callback;
    {
      std::lock_guard<std::mutex> lock(scanning_mutex_);
      if (!scanning_ || scan_callback_ == nullptr) return;
      callback = scan_callback_;
    }
    if (callback != nullptr) {
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
