// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Grid power source: Shelly Local Network Messaging (LNM).
 *
 * Joins the configured UDP multicast group and keeps the most recent grid
 * active power value reported by Shelly devices (energy meters, plugs, ...)
 * using their LNM status messages.
 *
 * Wire protocol and JSON payload are documented at:
 * - https://shelly-api-docs.shelly.cloud/gen2/General/LocalNetworkMessaging/
 * - https://shelly-api-docs.shelly.cloud/gen2/DynamicComponents/LNM/
 */
#include "ZeroExport_ShellyLNM.h"
#include "Configuration.h"
#include "NetworkSettings.h"
#include <ArduinoJson.h>

#undef TAG
static const char* TAG = "zeroexport";

ZeroExportShellyLnmClass ZeroExportShellyLnm;

void ZeroExportShellyLnmClass::start()
{
    const CONFIG_T& config = Configuration.get();

    if (!NetworkSettings.isConnected()) {
        ESP_LOGW(TAG, "Cannot start Shelly LNM listener: network not connected");
        return;
    }

    IPAddress addr;
    if (!addr.fromString(config.ZeroExport.ShellyLnm.GroupAddress)) {
        ESP_LOGE(TAG, "Invalid Shelly LNM multicast address: %s", config.ZeroExport.ShellyLnm.GroupAddress);
        return;
    }

    if (!_udp.beginMulticast(addr, config.ZeroExport.ShellyLnm.GroupPort)) {
        ESP_LOGE(TAG, "Failed to start Shelly LNM listener on %s:%u",
            config.ZeroExport.ShellyLnm.GroupAddress, config.ZeroExport.ShellyLnm.GroupPort);
        return;
    }

    strlcpy(_listenAddr, config.ZeroExport.ShellyLnm.GroupAddress, sizeof(_listenAddr));
    _listenPort = config.ZeroExport.ShellyLnm.GroupPort;
    _listening = true;

    ESP_LOGI(TAG, "Shelly LNM listener started on %s:%u", _listenAddr, _listenPort);
}

void ZeroExportShellyLnmClass::stop()
{
    if (_listening) {
        _udp.stop();
        _listening = false;
        ESP_LOGI(TAG, "Shelly LNM listener stopped");
    }
    // Do not let a quick disable/re-enable reuse a measurement from the
    // previous listener session. The new session must receive a fresh packet.
    _power = NAN;
    _lastUpdate = 0;
}

bool ZeroExportShellyLnmClass::isRunning() const
{
    return _listening;
}

void ZeroExportShellyLnmClass::loop()
{
    if (!_listening) {
        return;
    }
    pollUdp();
}

void ZeroExportShellyLnmClass::pollUdp()
{
    // Process all queued packets, keep only the most recent grid value
    int packetSize;
    while ((packetSize = _udp.parsePacket()) > 0) {
        static uint8_t buffer[1460];
        if (packetSize > static_cast<int>(sizeof(buffer))) {
            _udp.flush();
            ESP_LOGW(TAG, "Shelly LNM packet too large: %d", packetSize);
            continue;
        }

        const int len = _udp.read(buffer, sizeof(buffer));
        if (len > 0) {
            processPacket(buffer, static_cast<size_t>(len));
        }
    }
}

void ZeroExportShellyLnmClass::processPacket(const uint8_t* data, size_t len)
{
    // Shelly LNM wire protocol:
    //   [header (12 bytes)] [payload (payload_len bytes)] [meta (meta_len bytes)]
    //
    // Binary header (little-endian, packed):
    //   offset 0  size 2  magic        0x53 0x4C ("SL")
    //   offset 2  size 1  version      protocol version (currently 0)
    //   offset 3  size 1  payload_type 1 = status/event
    //   offset 4  size 2  payload_len  payload length in bytes (LE)
    //   offset 6  size 2  meta_len     meta block length (currently 0)
    //   offset 8  size 4  reserved
    //
    // For payload_type 1 the payload is a JSON object:
    //   {"device":"shellyproem50-...","ts":...,"status":{"em1:0":{"id":0,"voltage":241.8,"current":3.559,"act_power":291.5,...}}}

    if (len < 12) {
        ESP_LOGD(TAG, "LNM packet too short: %u", len);
        return;
    }

    if (data[0] != 0x53 || data[1] != 0x4C) {
        return; // Not a Shelly LNM message
    }

    const uint8_t payloadType = data[3];
    const uint16_t payloadLen = data[4] | (data[5] << 8);
    const uint16_t metaLen = data[6] | (data[7] << 8);

    if (payloadType != 1) {
        return; // Only status/event payloads carry the JSON we need
    }

    if (12u + payloadLen + metaLen > len || payloadLen == 0) {
        ESP_LOGD(TAG, "LNM truncated/empty packet: payload=%u meta=%u len=%u", payloadLen, metaLen, len);
        return;
    }

    JsonDocument doc;
    const DeserializationError err = deserializeJson(doc, reinterpret_cast<const char*>(data) + 12, payloadLen);
    if (err) {
        ESP_LOGD(TAG, "LNM JSON parse error: %s", err.c_str());
        return;
    }

    JsonObject status = doc["status"].as<JsonObject>();
    if (status.isNull()) {
        return;
    }

    for (JsonPair kv : status) {
        JsonObject component = kv.value().as<JsonObject>();
        if (component.isNull()) {
            continue;
        }

        if (!component["act_power"].is<float>()) {
            continue;
        }

        _power = component["act_power"].as<float>();
        _lastUpdate = millis();
        ESP_LOGD(TAG, "LNM grid power: %.1f W", _power);
        break; // Use the first component with a power value
    }
}

std::optional<float> ZeroExportShellyLnmClass::getPower() const
{
    if (std::isnan(_power) || _lastUpdate == 0 || millis() - _lastUpdate > POWER_TIMEOUT_MS) {
        return std::nullopt;
    }
    return _power;
}