#include "gadget_config.h"

#include <cstdlib>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>

namespace {

void require(bool condition, const char* message) {
    if (condition) return;
    std::cerr << message << '\n';
    std::exit(1);
}

bool rejects(const std::string& text) {
    std::istringstream input(text);
    try {
        (void)mtpadb::gadget::parse_gadget_config(input);
        return false;
    } catch (const std::invalid_argument&) {
        return true;
    }
}

}  // namespace

int main() {
    std::istringstream input(
        "# host identity\n"
        "[gadget]\n"
        "manufacturer = MTPADB Lab\n"
        "product=Virtual ADB\n"
        "serial=host-07\n"
        "vid=0x1209\n"
        "pid=4660\n"
        "udc=dummy_udc.test\n");
    const auto config = mtpadb::gadget::parse_gadget_config(input);
    require(config.manufacturer == "MTPADB Lab", "manufacturer was not parsed");
    require(config.product == "Virtual ADB", "product was not parsed");
    require(config.serial == "host-07", "serial was not parsed");
    require(config.vid == 0x1209, "hexadecimal VID was not parsed");
    require(config.pid == 4660, "decimal PID was not parsed");
    require(config.udc == "dummy_udc.test", "explicit UDC was not parsed");

    std::istringstream empty;
    const auto defaults = mtpadb::gadget::parse_gadget_config(empty);
    require(defaults.manufacturer == "MTPADB Project", "default manufacturer changed");
    require(defaults.product == "Virtual Android Debug Transport", "default product changed");
    require(defaults.serial == "MTPADB-0001", "default serial is not stable");
    require(defaults.vid == 0xffff && defaults.pid == 0xffff,
            "default IDs are not the documented local test IDs");
    require(!defaults.udc.has_value(), "default UDC must be dynamically discovered");

    require(rejects("[gadget]\nvid=0x10000\n"), "out-of-range VID was accepted");
    require(rejects("[gadget]\npid=0\n"), "zero PID was accepted");
    require(rejects("[gadget]\nvid=1\nvid=2\n"), "duplicate key was accepted");
}
