#include "rpc_service.h"

#include <cstddef>
#include <cstring>

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace mtpadb::mtpadbd {

int connect_mtprpcd_socket(const char* path) noexcept {
    if (path == nullptr || path[0] == '\0') return -1;

    sockaddr_un address{};
    const std::size_t path_length = std::strlen(path);
    if (path_length >= sizeof(address.sun_path)) return -1;

    const int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;

    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, path, path_length + 1);
    const socklen_t address_length = static_cast<socklen_t>(
            offsetof(sockaddr_un, sun_path) + path_length + 1);
    if (connect(fd, reinterpret_cast<const sockaddr*>(&address), address_length) != 0) {
        close(fd);
        return -1;
    }

    return fd;
}

int connect_mtprpcd_socket() noexcept {
    return connect_mtprpcd_socket("/dev/socket/mtprpcd-adb");
}

}  // namespace mtpadb::mtpadbd
