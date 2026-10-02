#include "gadget_config.h"
#include "gadget_manager.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

constexpr char kDefaultConfig[] = "/etc/mtpadb/host.conf";

mtpadb::gadget::GadgetConfig read_config(const std::string& path, bool explicitly_set) {
    std::ifstream input(path);
    if (input) return mtpadb::gadget::parse_gadget_config(input);
    if (explicitly_set || std::filesystem::exists(path)) {
        throw std::runtime_error("cannot read config file: " + path);
    }
    return {};
}

void print_usage() {
    std::cerr << "Usage: mtpadb-gadgetd run [--config PATH] | cleanup\n";
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        print_usage();
        return 2;
    }

    try {
        const std::string command = argv[1];
        if (command == "cleanup") {
            if (argc != 2) {
                print_usage();
                return 2;
            }
            mtpadb::gadget::cleanup_gadget();
            return 0;
        }
        if (command != "run") {
            print_usage();
            return 2;
        }

        std::string config_path = kDefaultConfig;
        bool explicitly_set = false;
        if (argc == 4 && std::string(argv[2]) == "--config") {
            config_path = argv[3];
            explicitly_set = true;
        } else if (argc != 2) {
            print_usage();
            return 2;
        }

        mtpadb::gadget::run_gadget(read_config(config_path, explicitly_set));
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "mtpadb-gadgetd: " << error.what() << '\n';
        return 1;
    }
}
