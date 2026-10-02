#include "client/adb_client.h"
#include "adb_unique_fd.h"
#include "rpc_client.h"
#include "socket_spec.h"

#include <fcntl.h>
#include <unistd.h>

#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>

namespace {

constexpr char kDeviceKeyDirectory[] = "/etc/mtpadb/devices";

void print_usage() {
    std::cerr << "Usage:\n"
                 "  mtpadb rpc --serial <serial> ping\n"
                 "  mtpadb rpc --serial <serial> echo <file>\n";
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 5 || std::string_view(argv[1]) != "rpc" ||
        std::string_view(argv[2]) != "--serial" || argv[3][0] == '\0') {
        print_usage();
        return 2;
    }

    const std::string_view operation(argv[4]);
    mtpadb::host::RpcOperation rpc_operation;
    unique_fd input_fd;
    int output_fd = -1;
    if (operation == "ping" && argc == 5) {
        rpc_operation = mtpadb::host::RpcOperation::kPing;
    } else if (operation == "echo" && argc == 6) {
        input_fd.reset(::open(argv[5], O_RDONLY | O_CLOEXEC));
        if (input_fd < 0) {
            std::cerr << "Unable to open echo input file.\n";
            return 1;
        }
        rpc_operation = mtpadb::host::RpcOperation::kEcho;
        output_fd = STDOUT_FILENO;
    } else {
        print_usage();
        return 2;
    }

    const char* configured_socket = std::getenv("ADB_SERVER_SOCKET");
    const std::string server_socket =
            configured_socket == nullptr ? "tcp:5037" : configured_socket;
    if (!is_local_socket_spec(server_socket)) {
        std::cerr << "mtpadb requires a local ADB server socket.\n";
        return 1;
    }
    adb_set_socket_spec(server_socket.c_str());
    adb_set_transport(kTransportAny, argv[3], 0);
    std::string adb_error;
    unique_fd rpc_fd(adb_connect("mtpadb:rpc", &adb_error));
    if (rpc_fd < 0) {
        std::cerr << "Unable to open mtpadb:rpc on the selected ADB transport";
        if (!adb_error.empty()) std::cerr << ": " << adb_error;
        std::cerr << '\n';
        return 1;
    }

    std::string rpc_error;
    if (!mtpadb::host::run_rpc(rpc_fd.get(), rpc_operation, input_fd.get(), output_fd,
                               kDeviceKeyDirectory, &rpc_error)) {
        std::cerr << (rpc_error.empty() ? "MTPADB RPC failed" : rpc_error) << '\n';
        return 1;
    }
    if (rpc_operation == mtpadb::host::RpcOperation::kPing) std::cout << "PONG\n";
    return 0;
}
