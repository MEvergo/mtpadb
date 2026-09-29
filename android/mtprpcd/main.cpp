#include "service.h"

#include <cutils/sockets.h>
#include <openssl/crypto.h>

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

namespace {

using mtpadb::protocol::crypto::Bytes;
using mtpadb::protocol::crypto::DeviceId;

constexpr char kSocketName[] = "mtprpcd-adb";
constexpr char kSocketPath[] = "/dev/socket/mtprpcd-adb";
constexpr std::size_t kDeviceIdBytes = 16;
constexpr std::size_t kDeviceKeyBytes = 32;
constexpr int kPrivateFileFlags = O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK;
constexpr int kDirectoryFlags = O_RDONLY | O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW;
static_assert(DeviceId{}.size() == kDeviceIdBytes);

class ScopedFd {
public:
    explicit ScopedFd(int fd = -1) noexcept : fd_(fd) {}
    ~ScopedFd() {
        if (fd_ >= 0) close(fd_);
    }

    ScopedFd(const ScopedFd&) = delete;
    ScopedFd& operator=(const ScopedFd&) = delete;

    int get() const noexcept { return fd_; }

private:
    int fd_;
};

class SecretBytes {
public:
    ~SecretBytes() {
        if (!value.empty()) OPENSSL_cleanse(value.data(), value.size());
    }

    Bytes value;
};

ScopedFd open_directory_at(int parent_fd, const char* name) {
    return ScopedFd(openat(parent_fd, name, kDirectoryFlags));
}

bool read_exact_file(int fd, std::uint8_t* destination, std::size_t size) noexcept {
    struct stat metadata {};
    if (fstat(fd, &metadata) != 0 || !S_ISREG(metadata.st_mode) || metadata.st_uid != 0 ||
        (metadata.st_mode & 0077) != 0 || metadata.st_size != static_cast<off_t>(size)) {
        return false;
    }

    std::size_t offset = 0;
    while (offset < size) {
        const ssize_t count = read(fd, destination + offset, size - offset);
        if (count > 0) {
            offset += static_cast<std::size_t>(count);
            continue;
        }
        if (count < 0 && errno == EINTR) continue;
        return false;
    }

    std::uint8_t extra_byte = 0;
    for (;;) {
        const ssize_t count = read(fd, &extra_byte, sizeof(extra_byte));
        if (count == 0) return true;
        if (count < 0 && errno == EINTR) continue;
        return false;
    }
}

bool load_provisioned_file(const char* name, std::uint8_t* destination, std::size_t size) {
    ScopedFd root_fd(open("/", kDirectoryFlags));
    if (root_fd.get() < 0) return false;
    ScopedFd data_fd = open_directory_at(root_fd.get(), "data");
    if (data_fd.get() < 0) return false;
    ScopedFd adb_fd = open_directory_at(data_fd.get(), "adb");
    if (adb_fd.get() < 0) return false;
    ScopedFd project_fd = open_directory_at(adb_fd.get(), "mtpadb");
    if (project_fd.get() < 0) return false;
    ScopedFd file_fd(openat(project_fd.get(), name, kPrivateFileFlags));
    if (file_fd.get() < 0) return false;
    return read_exact_file(file_fd.get(), destination, size);
}

bool load_identity(DeviceId& device_id, SecretBytes& device_key) {
    device_key.value.resize(kDeviceKeyBytes);
    return load_provisioned_file("device.id", device_id.data(), kDeviceIdBytes) &&
           load_provisioned_file("device.key", device_key.value.data(), kDeviceKeyBytes);
}

bool is_expected_listener(int fd) noexcept {
    int socket_type = 0;
    socklen_t option_size = sizeof(socket_type);
    if (getsockopt(fd, SOL_SOCKET, SO_TYPE, &socket_type, &option_size) != 0 ||
        socket_type != SOCK_STREAM) {
        return false;
    }

    int accepting = 0;
    option_size = sizeof(accepting);
    if (getsockopt(fd, SOL_SOCKET, SO_ACCEPTCONN, &accepting, &option_size) != 0 ||
        accepting != 1) {
        return false;
    }

    sockaddr_un address{};
    socklen_t address_size = sizeof(address);
    if (getsockname(fd, reinterpret_cast<sockaddr*>(&address), &address_size) != 0 ||
        address.sun_family != AF_UNIX) {
        return false;
    }
    constexpr std::size_t kPathOffset = offsetof(sockaddr_un, sun_path);
    constexpr std::size_t kExpectedPathBytes = sizeof(kSocketPath);
    return address_size == kPathOffset + kExpectedPathBytes &&
           std::memcmp(address.sun_path, kSocketPath, kExpectedPathBytes) == 0;
}

bool set_close_on_exec(int fd) noexcept {
    const int flags = fcntl(fd, F_GETFD, 0);
    return flags >= 0 && fcntl(fd, F_SETFD, flags | FD_CLOEXEC) == 0;
}

int run_daemon() {
    const int listener = android_get_control_socket(kSocketName);
    if (listener < 0 || !is_expected_listener(listener)) return 1;

    DeviceId device_id{};
    SecretBytes device_key;
    if (!load_identity(device_id, device_key)) return 1;

    for (;;) {
        pollfd readiness{listener, POLLIN, 0};
        int result;
        do {
            result = poll(&readiness, 1, -1);
        } while (result < 0 && errno == EINTR);
        if (result <= 0 || (readiness.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
            return 1;
        }

        const int connected_fd = accept(listener, nullptr, nullptr);
        if (connected_fd < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
            return 1;
        }
        if (!set_close_on_exec(connected_fd)) {
            close(connected_fd);
            continue;
        }
        mtpadb::mtprpcd::serve_connection(connected_fd, device_id, device_key.value);
    }
}

}  // namespace

int main() { return run_daemon(); }
