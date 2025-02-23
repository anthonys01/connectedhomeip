/*
 *
 *    Copyright (c) 2021-2022 Project CHIP Authors
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 *
 *        http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
 */

#include "ChipDeviceScanner.h"

#if CHIP_DEVICE_CONFIG_ENABLE_CHIPOBLE

#include <errno.h>
#include <pthread.h>

#include <lib/support/logging/CHIPLogging.h>
#include <platform/CHIPDeviceLayer.h>

#include "BluezObjectList.h"
#include "Types.h"

namespace chip {
namespace DeviceLayer {
namespace Internal {

namespace {

// Helper context for creating GDBusObjectManager with
// chip::DeviceLayer::GLibMatterContextInvokeSync()
struct GDBusCreateObjectManagerContext
{
    GDBusObjectManager * object = nullptr;
    // Cancellable passed to g_dbus_object_manager_client_new_for_bus_sync()
    // which later can be used to cancel the scan operation.
    GCancellable * cancellable = nullptr;

    GDBusCreateObjectManagerContext() : cancellable(g_cancellable_new()) {}
    ~GDBusCreateObjectManagerContext()
    {
        g_object_unref(cancellable);
        if (object != nullptr)
        {
            g_object_unref(object);
        }
    }
};

CHIP_ERROR MainLoopCreateObjectManager(GDBusCreateObjectManagerContext * context)
{
    // When creating D-Bus proxy object, the thread default context must be initialized. Otherwise,
    // all D-Bus signals will be delivered to the GLib global default main context.
    VerifyOrDie(g_main_context_get_thread_default() != nullptr);

    std::unique_ptr<GError, GErrorDeleter> err;
    context->object = g_dbus_object_manager_client_new_for_bus_sync(
        G_BUS_TYPE_SYSTEM, G_DBUS_OBJECT_MANAGER_CLIENT_FLAGS_NONE, BLUEZ_INTERFACE, "/",
        bluez_object_manager_client_get_proxy_type, nullptr /* unused user data in the Proxy Type Func */,
        nullptr /* destroy notify */, context->cancellable, &MakeUniquePointerReceiver(err).Get());
    VerifyOrReturnError(context->object != nullptr, CHIP_ERROR_INTERNAL,
                        ChipLogError(Ble, "Failed to get DBUS object manager for device scanning: %s", err->message));

    return CHIP_NO_ERROR;
}

/// Retrieve CHIP device identification info from the device advertising data
bool BluezGetChipDeviceInfo(BluezDevice1 & aDevice, chip::Ble::ChipBLEDeviceIdentificationInfo & aDeviceInfo)
{
    GVariant * serviceData = bluez_device1_get_service_data(&aDevice);
    VerifyOrReturnError(serviceData != nullptr, false);

    GVariant * dataValue = g_variant_lookup_value(serviceData, CHIP_BLE_UUID_SERVICE_STRING, nullptr);
    VerifyOrReturnError(dataValue != nullptr, false);

    size_t dataLen         = 0;
    const void * dataBytes = g_variant_get_fixed_array(dataValue, &dataLen, sizeof(uint8_t));
    VerifyOrReturnError(dataBytes != nullptr && dataLen >= sizeof(aDeviceInfo), false);

    memcpy(&aDeviceInfo, dataBytes, sizeof(aDeviceInfo));
    return true;
}

} // namespace

ChipDeviceScanner::ChipDeviceScanner(GDBusObjectManager * manager, BluezAdapter1 * adapter, GCancellable * cancellable,
                                     ChipDeviceScannerDelegate * delegate) :
    mManager(manager),
    mAdapter(adapter), mCancellable(cancellable), mDelegate(delegate)
{
    g_object_ref(mAdapter);
    g_object_ref(mCancellable);
    g_object_ref(mManager);
}

ChipDeviceScanner::~ChipDeviceScanner()
{
    StopScan();

    // mTimerExpired should only be set to true in the TimerExpiredCallback, which means we are in that callback
    // right now so there is no need to cancel the timer. Doing so would result in deadlock trying to aquire the
    // chip stack lock which we already currently have.
    if (!mTimerExpired)
    {
        // In case the timeout timer is still active
        DeviceLayer::PlatformMgr().LockChipStack();
        chip::DeviceLayer::SystemLayer().CancelTimer(TimerExpiredCallback, this);
        DeviceLayer::PlatformMgr().UnlockChipStack();
    }

    g_object_unref(mManager);
    g_object_unref(mCancellable);
    g_object_unref(mAdapter);

    mManager     = nullptr;
    mAdapter     = nullptr;
    mCancellable = nullptr;
    mDelegate    = nullptr;
}

std::unique_ptr<ChipDeviceScanner> ChipDeviceScanner::Create(BluezAdapter1 * adapter, ChipDeviceScannerDelegate * delegate)
{
    GDBusCreateObjectManagerContext context;
    CHIP_ERROR err;

    err = PlatformMgrImpl().GLibMatterContextInvokeSync(MainLoopCreateObjectManager, &context);
    VerifyOrExit(err == CHIP_NO_ERROR, ChipLogError(Ble, "Failed to create BLE object manager"));

    return std::make_unique<ChipDeviceScanner>(context.object, adapter, context.cancellable, delegate);

exit:
    return std::unique_ptr<ChipDeviceScanner>();
}

CHIP_ERROR ChipDeviceScanner::StartScan(System::Clock::Timeout timeout)
{
    ReturnErrorCodeIf(mIsScanning, CHIP_ERROR_INCORRECT_STATE);

    mIsScanning = true; // optimistic, to allow all callbacks to check this
    if (PlatformMgrImpl().GLibMatterContextInvokeSync(MainLoopStartScan, this) != CHIP_NO_ERROR)
    {
        ChipLogError(Ble, "Failed to schedule BLE scan start.");
        mIsScanning = false;
        return CHIP_ERROR_INTERNAL;
    }

    if (!mIsScanning)
    {
        ChipLogError(Ble, "Failed to start BLE scan.");
        return CHIP_ERROR_INTERNAL;
    }

    DeviceLayer::PlatformMgr().LockChipStack();
    CHIP_ERROR err = chip::DeviceLayer::SystemLayer().StartTimer(timeout, TimerExpiredCallback, static_cast<void *>(this));
    DeviceLayer::PlatformMgr().UnlockChipStack();

    if (err != CHIP_NO_ERROR)
    {
        ChipLogError(Ble, "Failed to schedule scan timeout.");
        StopScan();
        return err;
    }
    mTimerExpired = false;

    return CHIP_NO_ERROR;
}

void ChipDeviceScanner::TimerExpiredCallback(chip::System::Layer * layer, void * appState)
{
    ChipDeviceScanner * chipDeviceScanner = static_cast<ChipDeviceScanner *>(appState);
    chipDeviceScanner->MarkTimerExpired();
    chipDeviceScanner->mDelegate->OnScanError(CHIP_ERROR_TIMEOUT);
    chipDeviceScanner->StopScan();
}

CHIP_ERROR ChipDeviceScanner::StopScan()
{
    ReturnErrorCodeIf(!mIsScanning, CHIP_NO_ERROR);
    ReturnErrorCodeIf(mIsStopping, CHIP_NO_ERROR);
    mIsStopping = true;
    g_cancellable_cancel(mCancellable); // in case we are currently running a scan

    if (mObjectAddedSignal)
    {
        g_signal_handler_disconnect(mManager, mObjectAddedSignal);
        mObjectAddedSignal = 0;
    }

    if (mInterfaceChangedSignal)
    {
        g_signal_handler_disconnect(mManager, mInterfaceChangedSignal);
        mInterfaceChangedSignal = 0;
    }

    if (PlatformMgrImpl().GLibMatterContextInvokeSync(MainLoopStopScan, this) != CHIP_NO_ERROR)
    {
        ChipLogError(Ble, "Failed to schedule BLE scan stop.");
        return CHIP_ERROR_INTERNAL;
    }

    ChipDeviceScannerDelegate * delegate = this->mDelegate;
    // callback is explicitly allowed to delete the scanner (hence no more
    // references to 'self' here)
    delegate->OnScanComplete();

    return CHIP_NO_ERROR;
}

CHIP_ERROR ChipDeviceScanner::MainLoopStopScan(ChipDeviceScanner * self)
{
    GError * error = nullptr;

    if (!bluez_adapter1_call_stop_discovery_sync(self->mAdapter, nullptr /* not cancellable */, &error))
    {
        ChipLogError(Ble, "Failed to stop discovery %s", error->message);
        g_error_free(error);
    }
    self->mIsScanning = false;

    return CHIP_NO_ERROR;
}

void ChipDeviceScanner::SignalObjectAdded(GDBusObjectManager * manager, GDBusObject * object, ChipDeviceScanner * self)
{
    self->ReportDevice(bluez_object_get_device1(BLUEZ_OBJECT(object)));
}

void ChipDeviceScanner::SignalInterfaceChanged(GDBusObjectManagerClient * manager, GDBusObjectProxy * object,
                                               GDBusProxy * aInterface, GVariant * aChangedProperties,
                                               const gchar * const * aInvalidatedProps, ChipDeviceScanner * self)
{
    self->ReportDevice(bluez_object_get_device1(BLUEZ_OBJECT(object)));
}

void ChipDeviceScanner::ReportDevice(BluezDevice1 * device)
{
    if (device == nullptr)
    {
        return;
    }

    if (strcmp(bluez_device1_get_adapter(device), g_dbus_proxy_get_object_path(G_DBUS_PROXY(mAdapter))) != 0)
    {
        return;
    }

    chip::Ble::ChipBLEDeviceIdentificationInfo deviceInfo;

    if (!BluezGetChipDeviceInfo(*device, deviceInfo))
    {
        ChipLogDetail(Ble, "Device %s does not look like a CHIP device.", bluez_device1_get_address(device));
        return;
    }

    mDelegate->OnDeviceScanned(device, deviceInfo);
}

void ChipDeviceScanner::RemoveDevice(BluezDevice1 * device)
{
    if (device == nullptr)
    {
        return;
    }

    if (strcmp(bluez_device1_get_adapter(device), g_dbus_proxy_get_object_path(G_DBUS_PROXY(mAdapter))) != 0)
    {
        return;
    }

    chip::Ble::ChipBLEDeviceIdentificationInfo deviceInfo;

    if (!BluezGetChipDeviceInfo(*device, deviceInfo))
    {
        return;
    }

    const auto devicePath = g_dbus_proxy_get_object_path(G_DBUS_PROXY(device));
    GError * error        = nullptr;

    if (!bluez_adapter1_call_remove_device_sync(mAdapter, devicePath, nullptr, &error))
    {
        ChipLogDetail(Ble, "Failed to remove device %s: %s", StringOrNullMarker(devicePath), error->message);
        g_error_free(error);
    }
}

CHIP_ERROR ChipDeviceScanner::MainLoopStartScan(ChipDeviceScanner * self)
{
    GError * error = nullptr;

    self->mObjectAddedSignal = g_signal_connect(self->mManager, "object-added", G_CALLBACK(SignalObjectAdded), self);
    self->mInterfaceChangedSignal =
        g_signal_connect(self->mManager, "interface-proxy-properties-changed", G_CALLBACK(SignalInterfaceChanged), self);

    ChipLogProgress(Ble, "BLE removing known devices.");
    for (BluezObject & object : BluezObjectList(self->mManager))
    {
        self->RemoveDevice(bluez_object_get_device1(&object));
    }

    // Search for LE only.
    // Do NOT add filtering by UUID as it is done by the following kernel function:
    // https://github.com/torvalds/linux/blob/bdb575f872175ed0ecf2638369da1cb7a6e86a14/net/bluetooth/mgmt.c#L9258.
    // The function requires that devices advertise its services' UUIDs in UUID16/32/128 fields
    // while the Matter specification requires only FLAGS (0x01) and SERVICE_DATA_16 (0x16) fields
    // in the advertisement packets.
    GVariantBuilder filterBuilder;
    g_variant_builder_init(&filterBuilder, G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(&filterBuilder, "{sv}", "Transport", g_variant_new_string("le"));
    GVariant * filter = g_variant_builder_end(&filterBuilder);

    if (!bluez_adapter1_call_set_discovery_filter_sync(self->mAdapter, filter, self->mCancellable, &error))
    {
        // Not critical: ignore if fails
        ChipLogError(Ble, "Failed to set discovery filters: %s", error->message);
        g_clear_error(&error);
    }

    ChipLogProgress(Ble, "BLE initiating scan.");
    if (!bluez_adapter1_call_start_discovery_sync(self->mAdapter, self->mCancellable, &error))
    {
        ChipLogError(Ble, "Failed to start discovery: %s", error->message);
        g_error_free(error);

        self->mIsScanning = false;
        self->mDelegate->OnScanComplete();
    }

    return CHIP_NO_ERROR;
}

} // namespace Internal
} // namespace DeviceLayer
} // namespace chip

#endif // CHIP_DEVICE_CONFIG_ENABLE_CHIPOBLE
