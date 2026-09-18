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
#include "Datastore.h"
#include "MqttHandleHass.h"
#include "MqttSettings.h"
#include "defaults.h"
#include <Hoymiles.h>
#include <functional>

#undef TAG
static const char* TAG = "zeroexport";

#define ZEROEXPORT_LOOP_INTERVAL 500 // ms
#define ZEROEXPORT_PRODUCTION_MARGIN_MIN 200.0f // W, minimum headroom above measured production
#define ZEROEXPORT_PRODUCTION_MARGIN_PERCENT 0.20f // 20%, additional headroom above measured production
#define ZEROEXPORT_FAILSAFE_INTERVAL 300000UL // ms, keep non-persistent floor alive
#define ZEROEXPORT_KEEPALIVE_INTERVAL 300000UL // ms, well below the inverter's own non-persistent-limit timeout

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

    // Apply the persisted state on boot. In particular, if Zero-Export is
    // disabled after a reboot, this sets _pendingRelease so limits left by a
    // previous Zero-Export session are released instead of remaining active.
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

uint32_t ZeroExportClass::getMinimalProduction() const
{
    const CONFIG_T& config = Configuration.get();

    uint32_t count = 0;
    for (uint8_t i = 0; i < INV_MAX_COUNT; i++) {
        if (config.Inverter[i].Serial != 0) {
            count++;
        }
    }
    return config.ZeroExport.MinimalProduction * count;
}

void ZeroExportClass::applyConfig()
{
    const CONFIG_T& config = Configuration.get();
    const bool wasEnabled = _wasEnabled;

    // When Zero-Export gets disabled, release the limits we previously
    // applied so the inverters produce free at 100% again. Only release if
    // we were previously enabled (i.e. we may have touched the limits).
    if (!config.ZeroExport.Enabled && _wasEnabled) {
        releaseLimits();
    }
    _wasEnabled = config.ZeroExport.Enabled;

    // If the last command sequence was the release to 100%, regulation must
    // restart from nominal power, not from its usual 50 W startup floor.
    // Preserve the tracked limits for ordinary config changes while active.
    if (config.ZeroExport.Enabled && !wasEnabled && _limitsReleased) {
        _invStates.clear();
        _totalLimit = 0;
        _startupAtFullPower = true;
    }

    // If the DTU restarts while Zero-Export is disabled, previous non-persistent
    // limits from the last run may still be active on the inverters: schedule a
    // release until every inverter has acknowledged it. Re-enabling cancels any
    // pending release so a stray 100% command cannot land after regulation.
    _pendingRelease = !config.ZeroExport.Enabled;

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

bool ZeroExportClass::releaseLimits()
{
    // Send a relative 100% limit only to reachable inverters. An unreachable
    // inverter cannot receive the command, and enqueueing it would make the
    // shared radio queue retry/timeout and delay commands for other inverters.
    // _pendingRelease keeps the release active and retries it when the
    // inverter becomes reachable again. Since limit commands use
    // QueueInsertType::RemoveOldest, a repeated release replaces the still-
    // pending one instead of flooding the queue.
    // A relative non-persistent command restores the default behavior
    // without touching the inverter's stored (persistent) limit, and does
    // not require the device info (rated power), so it can also be sent to
    // inverters that never reported their max power.
    // Returns true only when the release was sent to every command-enabled
    // inverter while it was reachable, so the caller can keep retrying until
    // positive delivery is possible for all of them.
    bool allReleased = true;
    bool releaseCommandSent = false;
    for (uint8_t i = 0; i < Hoymiles.getNumInverters(); i++) {
        auto inv = Hoymiles.getInverterByPos(i);
        if (inv == nullptr) {
            continue;
        }
        if (!inv->getEnableCommands()) {
            // Commands are disabled for this inverter in the configuration:
            // Zero-Export never applied a limit to it, so there is nothing
            // to release. The radio layer would drop the command anyway.
            continue;
        }
        const bool reachable = inv->isReachable();
        if (!reachable) {
            allReleased = false;
            ESP_LOGW(TAG, "Release limit for %s skipped: inverter not reachable", inv->name());
            continue;
        }
        ESP_LOGI(TAG, "Release limit for %s: 100%%", inv->name());
        inv->sendActivePowerControlRequest(100, PowerLimitControlType::RelativNonPersistent);
        releaseCommandSent = true;
    }
    _limitsReleased = releaseCommandSent;
    return allReleased;
}

void ZeroExportClass::onNetworkEvent()
{
    // Always re-evaluate every source on any network change. start()
    // verifies that the network is actually connected.
    const CONFIG_T& config = Configuration.get();
    stopAllSources();
    if (config.ZeroExport.Enabled) {
        activeSource().start();
    }
}

void ZeroExportClass::sendKeepAlive()
{
    // Resend the exact limit already tracked for each controllable inverter,
    // without recalculating anything, so the values themselves are never
    // touched. Only prevents the inverter's own non-persistent-limit timeout
    // from silently reverting it to full power.
    if (millis() - _lastKeepAlive < ZEROEXPORT_KEEPALIVE_INTERVAL) {
        return;
    }
    _lastKeepAlive = millis();

    for (uint8_t i = 0; i < _invStates.size(); i++) {
        auto inv = Hoymiles.getInverterByPos(i);
        auto& st = _invStates[i];
        if (inv == nullptr || st.StartupPending || st.Limit <= 0) {
            continue;
        }
        if (!inv->isReachable() || !inv->getEnableCommands()) {
            continue;
        }
        ESP_LOGD(TAG, "Keep-alive limit for %s: %.0f W", inv->name(), st.Limit);
        inv->sendActivePowerControlRequest(st.Limit, PowerLimitControlType::AbsolutNonPersistent);
    }
}

void ZeroExportClass::applyGridUnavailableFailSafe()
{
    const uint8_t numInverters = Hoymiles.getNumInverters();
    if (_invStates.size() != numInverters) {
        _invStates.resize(numInverters);
    }

    if (!_gridFailSafeActive) {
        // Mark every inverter at the beginning of each outage. Reachability
        // is independent per inverter, so one global timestamp is not enough
        // to know whether each inverter received this outage's 50 W command.
        for (auto& state : _invStates) {
            state.FailSafePending = true;
        }
    }
    _gridFailSafeActive = true;

    bool commandSent = false;
    for (uint8_t i = 0; i < numInverters; i++) {
        auto inv = Hoymiles.getInverterByPos(i);
        auto& st = _invStates[i];

        if (inv == nullptr || !inv->isReachable() || !inv->getEnableCommands()) {
            // Do not reset the cached limit for an inverter that cannot
            // receive the fail-safe command. Its last commanded limit must
            // remain the source of truth until it becomes reachable again.
            continue;
        }

        const uint16_t maxPower = inv->DevInfo()->getMaxPower();
        if (maxPower == 0) {
            continue;
        }

        if (st.Serial != inv->serial()) {
            st.Serial = inv->serial();
            st.Limit = 0;
            st.StartupPending = true;
            st.FailSafePending = true;
        }

        const float safeLimit = std::min<float>(Configuration.get().ZeroExport.MinimalProduction, maxPower);
        const bool needsLimit = st.StartupPending || st.FailSafePending || st.Limit > safeLimit;
        const bool needsKeepAlive = _lastFailSafe == 0
            || millis() - _lastFailSafe >= ZEROEXPORT_FAILSAFE_INTERVAL;
        if (!needsLimit && !needsKeepAlive) {
            continue;
        }

        ESP_LOGW(TAG, "Grid source unavailable: limiting %s to %.0f W", inv->name(), safeLimit);
        inv->sendActivePowerControlRequest(safeLimit, PowerLimitControlType::AbsolutNonPersistent);
        st.Limit = safeLimit;
        st.StartupPending = false;
        st.StartupAtFullPower = false;
        st.FailSafePending = false;
        commandSent = true;
    }

    if (commandSent) {
        _lastFailSafe = millis();
        _totalLimit = 0;
        for (const auto& st : _invStates) {
            _totalLimit += st.Limit;
        }
    }
}

void ZeroExportClass::regulate()
{
    const CONFIG_T& config = Configuration.get();

    if (!config.ZeroExport.Enabled) {
        return;
    }

    ZeroExportGridSource& source = activeSource();

    // No grid value means no regulation can be performed. Check this before
    // touching inverter state or sending startup limit commands: there is no
    // reason to generate RF traffic while the grid source is unavailable.
    // The next valid source value will start the commanded-limit sequence.
    // (Re-)size state tracking first, so _totalLimit is never left at a stale
    // default when no inverters are present yet.
    const std::optional<float> gridPowerValue = source.getPower();
    if (!gridPowerValue.has_value()) {
        if (!_gridFailSafeActive) {
            ESP_LOGW(TAG, "No fresh grid value: applying minimum production fail-safe");
        }
        applyGridUnavailableFailSafe();
        return;
    }
    const float gridPower = gridPowerValue.value();

    // Grid power is available, so the fail-safe state can be cleared.
    _gridFailSafeActive = false;
    for (auto& state : _invStates) {
        state.FailSafePending = false;
    }

    const uint8_t numInverters = Hoymiles.getNumInverters();
    if (numInverters == 0) {
        _invStates.clear();
        _totalLimit = 0;
        return;
    }

    // (Re-)size state tracking. A new state starts with StartupPending so a
    // reboot never guesses the inverter's previous non-persistent limit.
    if (_invStates.size() != numInverters) {
        _invStates.resize(numInverters);
    }

    // Derive the regulation aggregates from the last limits requested by this
    // controller. OpenDTU's reported limits are deliberately not used here:
    // the grid error is the feedback signal.
    //
    // Startup/recovery rules:
    // - A DTU reboot with Zero-Export active starts each known inverter at
    //   50 W because the previous non-persistent command is not retained in
    //   Zero-Export memory.
    // - Re-enabling after releaseLimits() starts at nominal power because the
    //   last command sent was already a 100% release.
    // - A newly seen/replaced inverter starts at 50 W.
    // - A temporarily unreachable inverter keeps its last commanded limit and
    //   is included as fixed capacity when the other inverters regulate.
    // The startup command is sent only after the grid source has a value, so
    // an unavailable source cannot cause unnecessary inverter commands.
    float nominalTotal = 0;
    float fixedLimit = 0; // limits belonging to currently unreachable inverters
    float controllableNominal = 0;
    uint8_t controllableCount = 0;
    float totalLimit = 0;
    bool startupCommandSent = false;
    bool startupPending = false;

    for (uint8_t i = 0; i < numInverters; i++) {
        auto inv = Hoymiles.getInverterByPos(i);
        auto& st = _invStates[i];

        const uint16_t maxPower = (inv == nullptr) ? 0 : inv->DevInfo()->getMaxPower();
        if (maxPower == 0) {
            st.Serial = 0;
            st.Limit = 0;
            st.StartupPending = true;
            st.StartupAtFullPower = _startupAtFullPower;
            startupPending = true;
            continue;
        }

        if (st.Serial != inv->serial()) {
            st.Serial = inv->serial();
            st.Limit = 0;
            st.StartupPending = true;
            st.StartupAtFullPower = _startupAtFullPower;
        }

        const bool controllable = inv->isReachable() && inv->getEnableCommands();
        if (st.StartupPending) {
            if (!controllable) {
                startupPending = true;
                continue; // Initialize when the inverter becomes controllable.
            }

            const bool atFullPower = st.StartupAtFullPower;
            st.Limit = atFullPower
                ? maxPower
                : std::min<float>(config.ZeroExport.MinimalProduction, maxPower);
            st.StartupPending = false;
            st.StartupAtFullPower = false;
            _limitsReleased = false;
            startupCommandSent = true;
            ESP_LOGI(TAG, "Startup limit for %s: %.0f W%s", inv->name(), st.Limit,
                atFullPower ? " (after release)" : "");
            inv->sendActivePowerControlRequest(st.Limit, PowerLimitControlType::AbsolutNonPersistent);
            _lastKeepAlive = millis();
        }

        nominalTotal += maxPower;
        totalLimit += st.Limit;
        if (controllable) {
            controllableNominal += maxPower;
            controllableCount++;
        } else {
            // Keep a known command limit fixed while an inverter is offline or
            // commands are disabled (for example during the night).
            fixedLimit += st.Limit;
        }
    }

    _totalLimit = totalLimit;
    if (!startupPending) {
        _startupAtFullPower = false;
    }

    // Let startup commands apply before using their new limits for the first
    // regulation calculation. The next scheduled run uses the commanded minimal inverter power
    // values and the grid error to increase or decrease them.
    if (startupCommandSent) {
        return;
    }

    if (millis() - _lastRegulation < config.ZeroExport.UpdateInterval * 1000UL) {
        return; // Respect the configured regulation interval
    }
    _lastRegulation = millis();

    if (controllableCount == 0 || nominalTotal <= 0) {
        ESP_LOGD(TAG, "No controllable inverters available");
        return;
    }

    const uint32_t minProductionTotal = getMinimalProduction();
    const float setPoint = clampSetPoint(config.ZeroExport.SetPoint);

    // Regulation:
    //   missedPower = (grid - setpoint)   ; grid > setpoint -> too much export -> increase limit
    //                                     ; grid < setpoint -> importing     -> decrease limit
    float delta = gridPower - setPoint;

    // Source-adaptive correction: with a slow source (for example an MQTT
    // meter publishing every 10-30 seconds), the previous correction may not
    // have reached the grid measurement yet. Applying the full error again
    // would multiply the effective loop gain and can cause overshoot. Use the
    // measured source period to spread upward corrections over the source's
    // response time. Downward corrections stay immediate to reduce export.
    const uint32_t sourcePeriodMs = source.getUpdatePeriodMs();
    if (delta > 0 && sourcePeriodMs > 0) {
        const uint32_t loopLatencyMs = sourcePeriodMs + InverterResponseMs;
        const uint32_t regulationIntervalMs = config.ZeroExport.UpdateInterval * 1000UL;
        const float gain = std::min<float>(1.0f,
            static_cast<float>(regulationIntervalMs) / static_cast<float>(loopLatencyMs));
        delta = delta * gain;
        if (gain < 1.0f) {
            ESP_LOGW(TAG, "Slow grid source (period %" PRIu32 " ms): correction factor: %.0f%%",
                static_cast<uint32_t>(sourcePeriodMs), static_cast<double>(gain) * 100.0);
        }
    }

    // Do not let the requested limit drift far above the production currently
    // reported by the inverters. Keep at least 200 W of headroom so small
    // production values do not make the cap unnecessarily restrictive.
    // The cap's floor is "minimal production + margin", not the minimal
    // production itself: without the margin, a morning start (measured
    // production below the configured per-inverter floor, e.g. 8 x 40 W)
    // would clamp newTotalLimit to exactly _totalLimit and the limit could
    // never ramp up, because the inverters cannot produce more than their
    // currently applied limit. Adding the margin guarantees the loop can
    // always step out of a low-light start: floor 320 W -> cap 520 W, the
    // production follows the new limit, and each cycle lifts the cap again.
    const float currentProduction = std::max<float>(0, Datastore.getTotalAcPowerEnabled());
    const float productionMargin = std::max<float>(
        ZEROEXPORT_PRODUCTION_MARGIN_MIN,
        currentProduction * ZEROEXPORT_PRODUCTION_MARGIN_PERCENT);
    const float productionBase = std::max<float>(currentProduction, minProductionTotal);
    const float productionCap = std::min<float>(
        nominalTotal,
        productionBase + productionMargin);

    // Compute the new total limit by applying the delta to the current total limit.
    float newTotalLimit = _totalLimit + delta;
    newTotalLimit = std::min<float>(nominalTotal, std::max<float>(minProductionTotal, newTotalLimit));
    newTotalLimit = std::min<float>(newTotalLimit, productionCap);
    newTotalLimit = std::ceil(newTotalLimit);

    // If the adjustment is smaller than 1 W, consider it negligible.
    // Skip the update but still send a keep-alive if necessary.
    if (std::fabs(newTotalLimit - _totalLimit) < 1.0f) {
        ESP_LOGD(TAG, "Grid %.1f W, setpoint %.1f W -> no adjustment needed (total limit %.0f W)",
            gridPower, setPoint, _totalLimit);
        sendKeepAlive();
        return;
    }

    ESP_LOGI(TAG, "Grid %.1f W, setpoint %.1f W -> adjusting total limit from %.0f to %.0f W",
        gridPower, setPoint, _totalLimit, newTotalLimit);

    // Keep unreachable inverters fixed and distribute only the adjustable
    // portion proportionally to reachable inverters. This handles both a
    // temporary communication outage and an inverter that remains offline.
    const float adjustableNominal = std::max<float>(0, nominalTotal - fixedLimit);
    const float minimumReachable = std::max<float>(0, minProductionTotal - fixedLimit);
    const float adjustableTarget = std::min<float>(adjustableNominal,
        std::max<float>(minimumReachable, newTotalLimit - fixedLimit));

    float distributed = 0;
    for (uint8_t i = 0; i < numInverters; i++) {
        auto inv = Hoymiles.getInverterByPos(i);
        auto& st = _invStates[i];

        const uint16_t maxPower = (inv == nullptr) ? 0 : inv->DevInfo()->getMaxPower();
        if (maxPower == 0 || st.StartupPending || !inv->isReachable() || !inv->getEnableCommands()) {
            if (maxPower > 0 && !st.StartupPending) {
                // Known commanded limit, but currently not controllable: keep
                // it fixed in the total while other inverters compensate.
                distributed += st.Limit;
            }
            continue;
        }

        const float weight = maxPower / controllableNominal;
        const float minShare = std::min<float>(maxPower, minimumReachable * weight);
        float target = std::ceil(adjustableTarget * weight);
        // Never command an inverter below a hard floor. The configured total
        // minimum can be split into very small per-inverter shares; a too-low
        // absolute limit can make an inverter stop producing entirely.
        target = std::min<float>(maxPower,
            std::max<float>(std::max<float>(minShare, config.ZeroExport.MinimalProduction), target));

        // Correct rounding drift on the last relevant inverter is not
        // necessary: the error is at most a few watts and gets fixed by the
        // next regulation cycle.

        if (std::fabs(target - st.Limit) >= 1.0f) {
            // Never enqueue a power control command for an inverter that is
            // not reachable: the radio layer retries unacknowledged commands
            // for a long time, which blocks the command queue and delays
            // commands for all other inverters.
            if (!inv->isReachable()) {
                ESP_LOGW(TAG, "%s: limit update skipped, inverter not reachable", inv->name());
                distributed += st.Limit;
                continue;
            }
            ESP_LOGI(TAG, "%s: %.0f -> %.0f W (max %.0f W)",
                inv->name(), static_cast<double>(st.Limit), static_cast<double>(target), static_cast<double>(maxPower));
            inv->sendActivePowerControlRequest(target, PowerLimitControlType::AbsolutNonPersistent);
            _lastKeepAlive = millis();
        }
        st.Limit = target;
        distributed += target;
    }

    _totalLimit = distributed;
}

void ZeroExportClass::loop()
{
    const CONFIG_T& config = Configuration.get();

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

    // While disabled, retry releasing the limits: the release is also sent to
    // unreachable inverters, and the retry continues until it was positively
    // delivered to all of them. This covers the case where the DTU restarted
    // while Zero-Export was disabled but the inverters still carry a
    // non-persistent limit from a previous regulation run.
    if (!config.ZeroExport.Enabled && _pendingRelease
        && (millis() - _lastRegulation >= 10 * 1000UL)) {
        _pendingRelease = !releaseLimits();
        _lastRegulation = millis();
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

void ZeroExportClass::publishStatus()
{
    if (!MqttSettings.getConnected() || millis() - _lastStatusPublish < 5000) {
        return;
    }

    _lastStatusPublish = millis();
    const CONFIG_T& config = Configuration.get();

    MqttSettings.publish("dtu/zeroexport/status/enabled", String(config.ZeroExport.Enabled ? 1 : 0));
    MqttSettings.publish("dtu/zeroexport/status/setpoint", String(clampSetPoint(config.ZeroExport.SetPoint)));
    MqttSettings.publish("dtu/zeroexport/status/minimal_production", String(getMinimalProduction()));
    // Publish "unavailable" instead of skipping: a skipped publication leaves
    // the last retained value in place, so Home Assistant would keep showing
    // a stale grid power (e.g. after the source went offline). An explicit
    // "unavailable" payload correctly marks the sensor as unknown. A value
    // that is no longer fresh is treated exactly like the regulation does:
    // better an honest "unavailable" than a plausible number measured a long
    // time ago.
    const std::optional<float> gridPower = getGridPower();
    if (!gridPower.has_value()) {
        MqttSettings.publish("dtu/zeroexport/status/grid_power", "unavailable");
    } else {
        MqttSettings.publish("dtu/zeroexport/status/grid_power", String(*gridPower, 1));
    }
    MqttSettings.publish("dtu/zeroexport/status/production_limit", String(_totalLimit, 0));
}

void ZeroExportClass::subscribeTopics()
{
    const String prefix = MqttSettings.getPrefix();
    const String enabledTopic = prefix + "dtu/zeroexport/cmd/enabled";
    const String setPointTopic = prefix + "dtu/zeroexport/cmd/setpoint";

    MqttSettings.unsubscribe(enabledTopic);
    MqttSettings.unsubscribe(setPointTopic);

    ESP_LOGI(TAG, "Subscribe to Zero-Export MQTT topics: %s, %s",
        enabledTopic.c_str(), setPointTopic.c_str());

    MqttSettings.subscribe(enabledTopic, 0,
        std::bind(&ZeroExportClass::onMqttMessageEnabled, this,
            std::placeholders::_1, std::placeholders::_2,
            std::placeholders::_3, std::placeholders::_4));

    MqttSettings.subscribe(setPointTopic, 0,
        std::bind(&ZeroExportClass::onMqttMessageSetPoint, this,
            std::placeholders::_1, std::placeholders::_2,
            std::placeholders::_3, std::placeholders::_4));
}

bool ZeroExportClass::setEnabled(bool enabled)
{
    {
        auto guard = Configuration.getWriteGuard();
        auto& config = guard.getConfig();
        if (config.ZeroExport.Enabled == enabled)
        {
            return false;
        }
        config.ZeroExport.Enabled = enabled;
    }
    Configuration.write();
    _lastStatusPublish = 0;
    applyConfig();
    return true;
}

bool ZeroExportClass::setSetPoint(int32_t setPoint)
{
    const int32_t sanitized = clampSetPoint(setPoint);
    {
        auto guard = Configuration.getWriteGuard();
        auto& config = guard.getConfig();
        if (config.ZeroExport.SetPoint == sanitized)
        {
            return false;
        }
        config.ZeroExport.SetPoint = sanitized;
    }
    Configuration.write();
    _lastStatusPublish = 0;
    publishStatus();
    return true;
}

void ZeroExportClass::onMqttMessageEnabled(const espMqttClientTypes::MessageProperties& properties, const char* topic, const uint8_t* payload, const size_t len)
{
    ESP_LOGD(TAG, "Received Zero-Export enabled command on %s", topic);
    const std::string value(reinterpret_cast<const char*>(payload), len);
    const bool enable = (value == "1" || value == "true" || value == "ON" || value == "on");
    if (setEnabled(enable))
    {
        ESP_LOGI(TAG, "Zero-Export %s via MQTT", enable ? "enabled" : "disabled");
    }
}

void ZeroExportClass::onMqttMessageSetPoint(const espMqttClientTypes::MessageProperties& properties, const char* topic, const uint8_t* payload, const size_t len)
{
    ESP_LOGD(TAG, "Received Zero-Export setpoint command on %s", topic);
    try {
        const int32_t value = std::stol(std::string(reinterpret_cast<const char*>(payload), len));
        if (setSetPoint(value))
        {
            const CONFIG_T& config = Configuration.get();
            ESP_LOGI(TAG, "Zero-Export setpoint set to %" PRId32 " W via MQTT", config.ZeroExport.SetPoint);
        }
    } catch (const std::exception& e) {
        ESP_LOGW(TAG, "Invalid setpoint payload: %s", e.what());
    }
}