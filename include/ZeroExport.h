// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "ZeroExportGridSource.h"
#include <TaskSchedulerDeclarations.h>
#include <espMqttClient.h>
#include <memory>
#include <optional>
#include <vector>

class InverterAbstract;

class ZeroExportClass {
public:
    ZeroExportClass();
    // Register the scheduler task, network-event handler, and MQTT commands.
    void init(Scheduler& scheduler);

    // Restart the active grid source and reset cached regulation state after
    // a config change.
    void applyConfig();

    // Current grid power from the active source, or empty if unavailable.
    std::optional<float> getGridPower() const;
    // Sum of the absolute limits most recently requested by this controller.
    float getTotalLimit() const;
    // millis() timestamp of the last regulation calculation.
    uint32_t getLastRegulationRun() const;

    // Resolve the configured minimum, including automatic 100 W/inverter mode.
    uint32_t getMinimalProduction() const;

private:
    // Minimal per-inverter cache: everything else (rated power, reachability,
    // command enablement, reported limit) is read directly from the inverter
    // objects, which OpenDTU's poll loop keeps up to date.
    struct InverterState {
        uint64_t Serial = 0; // Identity at this position; detects reordered or changed inverters.
        float Limit = 0; // Last absolute limit requested by Zero-Export, in watts.
        // A new/unknown inverter starts at the safe 100 W floor. This is
        // only used at DTU startup, after a release, or when an inverter is
        // newly seen/replaced. A temporarily unreachable inverter keeps its
        // last commanded limit instead of being reset when it returns.
        bool StartupPending = true;
        // A release command sent 100% immediately before re-enabling
        // Zero-Export means startup must resume from nominal power instead of
        // sending the 100 W initialization command again.
        bool StartupAtFullPower = false;
    };

    // Select the source matching the configuration. Returns the source to
    // use, or nullptr if the configured source is not implemented/disabled.
    ZeroExportGridSource* activeSource() const;

    // Run the fast task, poll the grid source, and perform time-gated regulation.
    void loop();
    // Reconcile the active source with the current network state.
    void onNetworkEvent();
    // Reconcile the cached limits with the real inverter state on every call
    // (before the grid-value and interval gates: the reconciliation itself
    // is free of RF traffic) and perform the time-gated regulation.
    void regulate();
    // Resend the currently tracked limit to every controllable inverter
    // without changing it, so an inverter's own non-persistent-limit timeout
    // cannot revert it to full power during a grid-source outage or a long
    // period without any adjustment. Rate-limited by ZEROEXPORT_KEEPALIVE_INTERVAL.
    void sendKeepAlive();
    // Send a relative 100% limit to every command-enabled inverter,
    // including currently unreachable ones, releasing the limit control
    // previously applied by this controller. Called when Zero-Export gets
    // disabled so the inverters run free again. Returns true only when the
    // release was sent to every inverter while it was reachable, so the
    // caller can keep retrying until positive delivery is possible.
    bool releaseLimits();
    // Publish HA state topics at a low, fixed rate.
    void publishStatus();
    // Register MQTT commands used by HA.
    void subscribeTopics();
    void onMqttMessageEnabled(const espMqttClientTypes::MessageProperties& properties, const char* topic, const uint8_t* payload, size_t len);
    void onMqttMessageSetPoint(const espMqttClientTypes::MessageProperties& properties, const char* topic, const uint8_t* payload, size_t len);
    bool setEnabled(bool enabled);
    bool setSetPoint(int32_t setPoint);
    // Constrain a setpoint to the range accepted by the web UI and the HA
    // number entity (±10000 W), regardless of the channel it came from.
    // Pure function without instance state.
    static constexpr int32_t clampSetPoint(int32_t setPoint)
    {
        return setPoint > 10000 ? 10000 : (setPoint < -10000 ? -10000 : setPoint);
    }
    Task _loopTask; // 500 ms task used to poll the grid source and gate regulation.

    // Indexed by Hoymiles position, but each entry also stores Serial so a
    // changed inverter order cannot reuse another inverter's cached limit.
    std::vector<InverterState> _invStates;
    float _totalLimit = 0; // Sum of the limits currently tracked by Zero-Export.
    bool _wasEnabled = false; // True while regulation is active; detects disable to release limits.
    bool _pendingRelease = false; // A 100% release still needs to be sent (inverters not reachable yet).
    bool _limitsReleased = false; // The last Zero-Export commands released limits to 100%.
    bool _startupAtFullPower = false; // Next startup sequence begins at nominal power.

    uint32_t _lastRegulation = 0; // millis() timestamp of the last control calculation.
    uint32_t _lastKeepAlive = 0; // millis() timestamp of the last keep-alive limit resend.
    uint32_t _lastStatusPublish = 0; // MQTT status publication throttle.
    bool _mqttWasConnected = false; // Detect reconnects and MQTT configuration changes.
};

extern ZeroExportClass ZeroExport;
