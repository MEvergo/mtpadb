#pragma once

#include "wireless_control.h"
#include "wireless_control_protocol.h"

namespace mtpadb::mtpadbd {

// Dispatches a validated protocol request against the wireless control state.
// This is NOT an authentication boundary; callers must authenticate the peer
// (the listener requires SO_PEERCRED UID 0) before invoking it. The caller must
// protect and wipe any returned pairing_code.
void dispatch_wireless_control_request(const wireless_protocol::Request& request,
                                       WirelessControl& control,
                                       wireless_protocol::Response& response);

// The listener remains owned by the caller. Wireless endpoints are stopped
// whenever this blocking service loop exits.
void run_wireless_control_server(int listening_fd, WirelessControl& control);

}  // namespace mtpadb::mtpadbd
