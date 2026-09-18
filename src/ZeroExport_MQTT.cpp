// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Grid power source: MQTT topic.
 *
 * Subscribes to the configured topic on OpenDTU's existing MQTT client and
 * keeps the most recent grid power value published there. The payload may
 * be a raw numeric value or a JSON object with a power field (Shelly EM /
 * 3EM style status payloads are supported, like in YaSolR).
 */
#include "ZeroExport_MQTT.h"
#include "Configuration.h"
#include "MqttSettings.h"
#include <ArduinoJson.h>

#undef TAG
static const char* TAG = "zeroexport";

ZeroExportMqttClass ZeroExportMqtt;

ZeroExportMqttClass::ZeroExportMqttClass()
    : ZeroExportGridSource(60 * 1000UL)
{
}

void ZeroExportMqttClass::start()
{
    const CONFIG_T& config = Configuration.get();
    const char* topic = config.ZeroExport.Mqtt.GridPowerTopic;

    if (strnlen(topic, sizeof(config.ZeroExport.Mqtt.GridPowerTopic)) == 0) {
        ESP_LOGW(TAG, "Cannot start Zero-Export MQTT source: no grid power topic configured");
        return;
    }

    // Idempotent: the subscription is registered in the shared client's
    // parser and survives broker reconnects, so a repeated start() must not
    // register duplicate callbacks. The controller calls start() whenever
    // isRunning() is false, which also happens while the broker is offline
    // even though the subscription is still registered.
    if (_subscribed) {
        if (strcmp(_subscribedTopic, topic) == 0) {
            return;
        }
        stop(); // Topic changed: unsubscribe the previous one first.
    }

    // The subscription is registered on the shared MQTT client and survives
    // its reconnects (MqttSettings resubscribes on connect). Subscribe with
    // the current topic only.
    MqttSettings.subscribe(topic, 0,
        std::bind(&ZeroExportMqttClass::onMqttMessage, this,
            std::placeholders::_1, std::placeholders::_2,
            std::placeholders::_3, std::placeholders::_4));

    strlcpy(_subscribedTopic, topic, sizeof(_subscribedTopic));
    _subscribed = true;

    ESP_LOGI(TAG, "Zero-Export MQTT source listening on topic: %s", _subscribedTopic);
}

void ZeroExportMqttClass::stop()
{
    if (_subscribed) {
        MqttSettings.unsubscribe(_subscribedTopic);
        _subscribed = false;
    }
    // Do not let a quick disable/re-enable reuse a measurement from the
    // previous subscription; the new session must receive a fresh value.
    clearPower();
}

bool ZeroExportMqttClass::isRunning() const
{
    // The subscription is registered on the shared client: it only yields
    // values while that client is connected.
    return _subscribed && MqttSettings.getConnected();
}

void ZeroExportMqttClass::loop()
{
    // Nothing to do: values arrive through the MQTT client callbacks.
}

void ZeroExportMqttClass::onMqttMessage(const espMqttClientTypes::MessageProperties& properties, const char* topic, const uint8_t* payload, const size_t len)
{
    if (properties.retain) {
        // Ignore retained messages: they are historical and can be arbitrarily
        // old. Only live (non-retained) publications drive the regulation.
        return;
    }

    float p = NAN;
    if (!parsePower(payload, len, p)) {
        ESP_LOGW(TAG, "Zero-Export MQTT payload on %s could not be parsed as grid power", topic);
        return;
    }

    setPower(p);
    ESP_LOGI(TAG, "Grid Power from MQTT: %.1f W", p);
}

bool ZeroExportMqttClass::parsePower(const uint8_t* payload, const size_t len, float& outPower)
{
    if (payload == nullptr || len == 0) {
        return false;
    }

    // Trim whitespace so " 123.4 " and "\n123.4" are accepted.
    const char* begin = reinterpret_cast<const char*>(payload);
    const char* end = begin + len;
    while (begin < end && isspace(static_cast<unsigned char>(*begin))) {
        begin++;
    }
    while (end > begin && isspace(static_cast<unsigned char>(end[-1]))) {
        end--;
    }
    if (begin == end) {
        return false;
    }

    if (*begin == '{') {
        // JSON object: accept the common Shelly EM / 3EM power fields, in
        // the same preference order as the YaSolR implementation.
        JsonDocument doc;
        const DeserializationError err = deserializeJson(doc, begin, end - begin);
        if (err != DeserializationError::Ok) {
            return false;
        }

        const float p = doc["act_power"] | (doc["total_act_power"] | NAN);
        if (std::isnan(p)) {
            return false;
        }
        outPower = p;
        return true;
    }

    // Plain numeric value.
    char* parseEnd = nullptr;
    const float p = strtof(begin, &parseEnd);
    if (parseEnd == begin || std::isnan(p)) {
        return false;
    }
    outPower = p;
    return true;
}
