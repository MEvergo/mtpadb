#include "gadget_config.h"

#include <charconv>
#include <stdexcept>
#include <string_view>
#include <system_error>

namespace mtpadb::gadget {
namespace {

std::string_view trim(std::string_view value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos) return {};
    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

std::uint16_t parse_id(std::string_view value, std::string_view key) {
    int base = 10;
    if (value.starts_with("0x") || value.starts_with("0X")) {
        value.remove_prefix(2);
        base = 16;
    }
    if (value.empty()) throw std::invalid_argument(std::string("empty ") + std::string(key));

    unsigned int parsed = 0;
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), parsed, base);
    if (error != std::errc{} || end != value.data() + value.size() || parsed == 0 || parsed > 0xffff) {
        throw std::invalid_argument(std::string("invalid ") + std::string(key));
    }
    return static_cast<std::uint16_t>(parsed);
}

void validate_usb_string(std::string_view value, std::string_view key) {
    if (value.empty() || value.size() > 126 || value.find('\0') != std::string_view::npos) {
        throw std::invalid_argument(std::string("invalid ") + std::string(key));
    }
}

}  // namespace

GadgetConfig parse_gadget_config(std::istream& input) {
    GadgetConfig config;
    bool in_gadget_section = false;
    bool saw_gadget_section = false;
    bool manufacturer_seen = false;
    bool product_seen = false;
    bool serial_seen = false;
    bool vid_seen = false;
    bool pid_seen = false;
    bool udc_seen = false;
    std::string line;

    while (std::getline(input, line)) {
        std::string_view text = trim(line);
        if (text.empty() || text.starts_with('#') || text.starts_with(';')) continue;
        if (text.front() == '[' && text.back() == ']') {
            if (text != "[gadget]" || saw_gadget_section) {
                throw std::invalid_argument("expected one [gadget] section");
            }
            in_gadget_section = true;
            saw_gadget_section = true;
            continue;
        }
        if (!in_gadget_section) throw std::invalid_argument("key outside [gadget] section");

        const auto equals = text.find('=');
        if (equals == std::string_view::npos) throw std::invalid_argument("expected key=value");
        const std::string_view key = trim(text.substr(0, equals));
        const std::string_view value = trim(text.substr(equals + 1));

        if (key == "manufacturer") {
            if (manufacturer_seen) throw std::invalid_argument("duplicate manufacturer");
            manufacturer_seen = true;
            config.manufacturer = value;
        } else if (key == "product") {
            if (product_seen) throw std::invalid_argument("duplicate product");
            product_seen = true;
            config.product = value;
        } else if (key == "serial") {
            if (serial_seen) throw std::invalid_argument("duplicate serial");
            serial_seen = true;
            config.serial = value;
        } else if (key == "vid") {
            if (vid_seen) throw std::invalid_argument("duplicate vid");
            vid_seen = true;
            config.vid = parse_id(value, "vid");
        } else if (key == "pid") {
            if (pid_seen) throw std::invalid_argument("duplicate pid");
            pid_seen = true;
            config.pid = parse_id(value, "pid");
        } else if (key == "udc") {
            if (udc_seen) throw std::invalid_argument("duplicate udc");
            udc_seen = true;
            if (!value.empty()) config.udc = value;
        } else {
            throw std::invalid_argument("unknown gadget key");
        }
    }

    validate_usb_string(config.manufacturer, "manufacturer");
    validate_usb_string(config.product, "product");
    validate_usb_string(config.serial, "serial");
    return config;
}

}  // namespace mtpadb::gadget
