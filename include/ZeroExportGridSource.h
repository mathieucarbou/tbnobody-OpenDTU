// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>
#include <optional>

// Abstract grid power source used by the Zero-Export controller.
//
// A source receives the current grid power from some device (power meter,
// energy sensor, ...) and exposes the most recent value to the controller.
// Implementations own their transport (UDP socket, HTTP client, ...) and
// are expected to update their value asynchronously; the controller only
// polls them and never blocks.
class ZeroExportGridSource {
public:
    virtual ~ZeroExportGridSource() = default;

    // Prepare the source (open sockets, join groups, ...). Called whenever
    // the configuration changes or the network comes up. Safe to call when
    // already started.
    virtual void start() = 0;
    // Release the transport resources. Safe to call when already stopped.
    virtual void stop() = 0;
    // Whether the transport is currently up (socket joined / client
    // connected). Used by the controller to self-heal a failed start.
    virtual bool isRunning() const = 0;
    // Called on every controller tick. The implementation may drain received
    // packets, fetch data, or perform any other non-blocking maintenance.
    virtual void loop() = 0;

    // Current grid power in watts, or empty when no value is available or the
    // implementation-specific freshness timeout has expired.
    virtual std::optional<float> getPower() const = 0;
};