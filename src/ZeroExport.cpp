// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Zero-Export controller.
 *
 * Reads the grid power reported by a configured grid source (see
 * ZeroExportGridSource) and regulates the inverter power limits so that
 * the grid power stays around a configurable setpoint (default: 0 W,
 * i.e. no export to / no import from the grid).
 *
 * The regulation only depends on the incoming grid value, the setpoint and
 * the limits currently applied to the inverters. It does NOT take the
 * inverters' production values into account, because these can be outdated.
 */
#include "ZeroExport.h"
#include "ZeroExport_ShellyLNM.h"
#include "ZeroExport_MQTT.h"
#include "Configuration.h"
#include "MqttHandleHass.h"
#include "MqttSettings.h"
#include "defaults.h"
#include <Hoymiles.h>
#include <functional>

#undef TAG
static const char* TAG = "zeroexport";

// rendezvous guard from the main-loop task, which would self-deadlock.
static CONFIG_T& mutableConfig()
{
    return const_cast<CONFIG_T&>(Configuration.get());
}

#define ZEROEXPORT_LOOP_INTERVAL 500 // ms

ZeroExportClass ZeroExport;

ZeroExportClass::ZeroExportClass()
    : _loopTask(ZEROEXPORT_LOOP_INTERVAL * TASK_MILLISECOND, TASK_FOREVER, std::bind(&ZeroExportClass::loop, this))
{
}

void ZeroExportClass::init(Scheduler& scheduler)
{
    NetworkSettings.onEvent(std::bind(&ZeroExportClass::onNetworkEvent, this));
    subscribeTopics();

    scheduler.addTask(_loopTask);
    _loopTask.enable();

    // Apply the persisted state on boot (start the configured grid source when
    // enabled). A previous Zero-Export session's non-persistent limits are
    // handled by the inverters' own timeout; with the module disabled nothing
    // is commanded here.
    applyConfig();
}

ZeroExportGridSource& ZeroExportClass::activeSource() const
{
    // The grid source is selected by the configuration; adding a new source
    // means adding a case here and an implementation of
    // ZeroExportGridSource.
    const CONFIG_T& config = Configuration.get();
    switch (config.ZeroExport.Source) {
    case ZEROEXPORT_SOURCE_SHELLY_LNM:
        return ZeroExportShellyLnm;
    case ZEROEXPORT_SOURCE_MQTT:
        return ZeroExportMqtt;
    default:
        ESP_LOGE(TAG, "Invalid ZeroExport source: %u", config.ZeroExport.Source);
        return ZeroExportShellyLnm; // Fallback to a valid source to satisfy the reference return type
    }
}

void ZeroExportClass::stopAllSources()
{
    // All grid sources are singletons; stop them all so that switching the
    // configured source (or editing its settings) never leaves the previous
    // one running: an open UDP socket would keep consuming packets and an
    // active MQTT subscription would keep firing its callback uselessly.
    ZeroExportShellyLnm.stop();
    ZeroExportMqtt.stop();

}

void ZeroExportClass::applyConfig()
{
    const CONFIG_T& config = Configuration.get();

    // When Zero-Export gets disabled, release the limits we previously
    // applied so the inverters produce free at 100% again. Only release if
    // we were previously enabled (i.e. we may have touched the limits).
    // The check must run before _wasEnabled is updated.
    if (!config.ZeroExport.Enabled && _wasEnabled) {
        releaseLimits();
    }
    _wasEnabled = config.ZeroExport.Enabled;

    if (config.ZeroExport.Enabled) {
        // Do not let an unsent release from the disabled state arrive after
        // regulation has been re-enabled. A release already sent remains
        // represented by StartupAtFullPower for that inverter.
        for (auto& state : _invStates) {
            state.ReleasePending = false;
        }
    }

    // Stop every grid source, then start the configured one: switching the
    // source (or editing its settings) must never leave the previous source
    // running (open socket, active subscription, stale measurement).
    stopAllSources();
    if (config.ZeroExport.Enabled) {
        activeSource().start();
    }

    publishStatus();
    MqttHandleHass.forceUpdate();
}

void ZeroExportClass::processPendingConfigChanges()
{
    // Apply changes requested from other task contexts: the MQTT command
    // handlers (MQTT client task), the Web API (async_tcp task) and the
    // network event handler (WiFi event task). The MQTT changes are live
    // updates: written to the in-RAM configuration so every reader
    // (regulation loop, status topics, Web API) sees them, but NOT persisted
    // to the configuration file — an automation can change them frequently
    // and that must not wear the flash. A reboot restores the values the user
    // configured through the Web UI.
    // The pending flags are exchanged atomically, so a command arriving while
    // this runs is never lost and is applied on the next loop pass. The
    // values are compared here, at apply time, so a command received while
    // an earlier one is still pending still wins.
    if (_mqttEnabledPending.exchange(false)) {
        const bool enable = _mqttEnabledValue.load();
        if (Configuration.get().ZeroExport.Enabled != enable) {
            mutableConfig().ZeroExport.Enabled = enable;
            ESP_LOGI(TAG, "Zero-Export %s via MQTT", enable ? "enabled" : "disabled");
            _lastStatusPublish = 0; // Publish the new state immediately, not on the next 5 s throttle
            applyConfig(); // Releases the limits (disable) or restarts the source (enable).
        }
    }
    if (_mqttSetPointPending.exchange(false)) {
        const int32_t sanitized = _mqttSetPointValue.load();
        if (Configuration.get().ZeroExport.SetPoint != sanitized) {
            mutableConfig().ZeroExport.SetPoint = sanitized;
            ESP_LOGI(TAG, "Zero-Export setpoint set to %" PRId32 " W via MQTT", sanitized);
            // A setpoint change alone does not restart the grid source.
            _lastStatusPublish = 0;
        }
    }
    // The Web API persists its configuration values itself (from its own
    // task, under the write guard) and only requests the side effects here:
    // restarting the grid source and releasing limits send radio commands
    // and therefore belong to the main-loop task.
    if (_applyConfigPending.exchange(false)) {
        applyConfig();
    }
    // A network event (disconnect/reconnect) was observed: re-evaluate every
    // source. start() verifies that the network is actually connected, so a
    // reconnect restarts the source and a disconnect stops them all. Sources
    // also self-heal below when they notice they are not running.
    if (_networkRestartPending.exchange(false)) {
        stopAllSources();
        if (Configuration.get().ZeroExport.Enabled) {
            activeSource().start();
        }
    }
}

void ZeroExportClass::onNetworkEvent()
{
    // Runs in the WiFi event task. Stopping/starting the grid sources touches
    // a UDP socket (Shelly LNM) and subscribes on the MQTT client, neither of
    // which is safe to use concurrently with the main loop, which drives
    // them. Only record the event; the Zero-Export loop reconciles the
    // sources with the network state.
    _networkRestartPending.store(true);
}

void ZeroExportClass::loop()
{
    const CONFIG_T& config = Configuration.get();

    // Apply configuration changes requested by the MQTT command handlers and
    // the Web API before anything else, so this loop never observes a config
    // value the pending request would change (in particular the enabled
    // flag, which decides whether limits must be released below).
    processPendingConfigChanges();

    // Keep the active grid source in sync with the configuration: stop it
    // when the module is disabled, (re-)start it when it should run but is
    // not running (self-healing in case the socket could not be opened yet,
    // e.g. the network was not ready at the last start attempt).
    if (!config.ZeroExport.Enabled) {
        // Stop every source: the module is disabled, and a source that
        // was switched away from earlier must not keep running either.
        stopAllSources();
    } else {
        ZeroExportGridSource& source = activeSource();
        if (!source.isRunning()) {
            source.start();
        }
        source.loop();
    }

    // regulate() first reconciles the cached limits with the real inverter
    // state on every call, then performs the time-gated regulation.
    regulate();

    const bool mqttConnected = MqttSettings.getConnected();
    if (mqttConnected && !_mqttWasConnected) {
        // Re-register using the current MQTT base topic. This matters when
        // the MQTT configuration was changed and the client reconnected.
        subscribeTopics();
    }
    _mqttWasConnected = mqttConnected;

    publishStatus();
}
