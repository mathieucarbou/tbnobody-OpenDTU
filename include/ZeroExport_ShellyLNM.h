// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "ZeroExportGridSource.h"
#include <WiFiUdp.h>
#include <optional>

// Grid power source receiving Shelly Local Network Messaging (LNM) status
// messages over UDP multicast.
//
// Wire protocol and JSON payload are documented at:
// - https://shelly-api-docs.shelly.cloud/gen2/General/LocalNetworkMessaging/
// - https://shelly-api-docs.shelly.cloud/gen2/DynamicComponents/LNM/
//
// The multicast group and port come from the Zero-Export configuration
// (ZeroExport.ShellyLnm.GroupAddress / GroupPort).
class ZeroExportShellyLnmClass : public ZeroExportGridSource {
public:
    void start() override;
    void stop() override;
    bool isRunning() const override;
    void loop() override;

    std::optional<float> getPower() const override;

private:
    // Drain queued datagrams without blocking the scheduler.
    void pollUdp();
    // Validate and decode one Shelly LNM datagram.
    void processPacket(const uint8_t* data, size_t len);

    WiFiUDP _udp; // Multicast socket.
    bool _listening = false; // Whether the multicast group is joined.
    char _listenAddr[16] = { 0 }; // Endpoint currently joined, for diagnostics.
    uint16_t _listenPort = 0; // Endpoint currently joined, for diagnostics.

    float _power = NAN; // Most recent accepted act_power value, in watts.
    uint32_t _lastUpdate = 0; // millis() timestamp for _power.
    static constexpr uint32_t POWER_TIMEOUT_MS = 10 * 1000UL;
};

extern ZeroExportShellyLnmClass ZeroExportShellyLnm;