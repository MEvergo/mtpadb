#pragma once

#include <cstdint>
#include <istream>
#include <optional>
#include <string>

namespace mtpadb::gadget {

struct GadgetConfig {
    std::string manufacturer = "MTPADB Project";
    std::string product = "Virtual Android Debug Transport";
    std::string serial = "MTPADB-0001";
    std::uint16_t vid = 0xffff;
    std::uint16_t pid = 0xffff;
    std::optional<std::string> udc;
};

GadgetConfig parse_gadget_config(std::istream& input);

}  // namespace mtpadb::gadget
