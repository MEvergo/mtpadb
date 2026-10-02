#include "wireless_control.h"

#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <string>
#include <utility>

namespace mtpadb::mtpadbd {
namespace {

constexpr std::size_t kMaximumPublicKeySize = 16 * 1024;
constexpr std::size_t kMaximumPeerIdSize = 128;
constexpr std::size_t kMaximumPeerCount = 1024;
constexpr auto kPairingWindow = std::chrono::seconds(120);

class ScopedFd {
  public:
    explicit ScopedFd(int fd = -1) noexcept : fd_(fd) {}
    ~ScopedFd() {
        if (fd_ >= 0) ::close(fd_);
    }

    ScopedFd(const ScopedFd&) = delete;
    ScopedFd(ScopedFd&& other) noexcept : fd_(other.release()) {}
    ScopedFd& operator=(ScopedFd&& other) noexcept {
        if (this != &other) {
            if (fd_ >= 0) ::close(fd_);
            fd_ = other.release();
        }
        return *this;
    }

    ScopedFd& operator=(const ScopedFd&) = delete;

    int get() const noexcept { return fd_; }
    int release() noexcept {
        const int fd = fd_;
        fd_ = -1;
        return fd;
    }

  private:
    int fd_;
};

bool is_peer_id(std::string_view peer_id) noexcept {
    if (peer_id.empty() || peer_id.size() > kMaximumPeerIdSize) return false;
    const auto is_alphanumeric = [](char ch) {
        return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
               (ch >= '0' && ch <= '9');
    };
    if (!is_alphanumeric(peer_id.front())) return false;
    return std::all_of(peer_id.begin(), peer_id.end(), [&](char ch) {
        return is_alphanumeric(ch) || ch == '-' || ch == '_' || ch == '.';
    });
}

bool is_pairing_code(std::string_view pairing_code) noexcept {
    return pairing_code.size() == 6 &&
           std::all_of(pairing_code.begin(), pairing_code.end(),
                       [](char digit) { return digit >= '0' && digit <= '9'; });
}

bool is_local_numeric_address(std::string_view address) {
    if (address.empty() || address.find('\0') != std::string_view::npos) return false;
    const std::string numeric_address(address);
    in_addr ipv4{};
    in6_addr ipv6{};
    int family = AF_UNSPEC;
    const void* requested_address = nullptr;
    if (::inet_pton(AF_INET, numeric_address.c_str(), &ipv4) == 1) {
        if (ipv4.s_addr == htonl(INADDR_ANY)) return false;
        family = AF_INET;
        requested_address = &ipv4;
    } else if (::inet_pton(AF_INET6, numeric_address.c_str(), &ipv6) == 1) {
        if (IN6_IS_ADDR_UNSPECIFIED(&ipv6)) return false;
        family = AF_INET6;
        requested_address = &ipv6;
    } else {
        return false;
    }

    ifaddrs* interfaces = nullptr;
    if (::getifaddrs(&interfaces) != 0) return false;
    bool found = false;
    for (const ifaddrs* current = interfaces; current != nullptr && !found;
         current = current->ifa_next) {
        if (current->ifa_addr == nullptr || current->ifa_addr->sa_family != family) continue;
        if (family == AF_INET) {
            const auto* local = reinterpret_cast<const sockaddr_in*>(current->ifa_addr);
            found = std::memcmp(&local->sin_addr, requested_address, sizeof(in_addr)) == 0;
        } else {
            const auto* local = reinterpret_cast<const sockaddr_in6*>(current->ifa_addr);
            found = std::memcmp(&local->sin6_addr, requested_address, sizeof(in6_addr)) == 0;
        }
    }
    ::freeifaddrs(interfaces);
    return found;
}

bool valid_private_peer_file(int fd) noexcept {
    struct stat status {};
    return ::fstat(fd, &status) == 0 && S_ISREG(status.st_mode) &&
           status.st_uid == ::geteuid() && (status.st_mode & 0077) == 0 &&
           status.st_size > 0 && static_cast<std::uint64_t>(status.st_size) <= kMaximumPublicKeySize;
}

bool write_all(int fd, std::string_view contents) noexcept {
    std::size_t offset = 0;
    while (offset < contents.size()) {
        const ssize_t written = ::write(fd, contents.data() + offset, contents.size() - offset);
        if (written < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (written == 0) return false;
        offset += static_cast<std::size_t>(written);
    }
    return true;
}

}  // namespace

WirelessControl::WirelessControl(WirelessRuntime& runtime,
                                 std::filesystem::path peer_store_directory)
    : runtime_(runtime), peer_store_directory_(std::move(peer_store_directory)) {}

WirelessControl::~WirelessControl() { stop(); }

bool WirelessControl::start(const Options& options, Clock::time_point now) {
    if (!is_pairing_code(options.pairing_code) ||
        !is_local_numeric_address(options.bind_address)) {
        return false;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    if (enabled_) return false;

    const auto pairing_port = runtime_.open_pairing_listener(
        options.bind_address, options.pairing_port, options.pairing_code);
    if (!pairing_port || *pairing_port == 0) {
        runtime_.close_pairing_listener();
        return false;
    }

    const auto connect_port = runtime_.open_connect_listener(options.bind_address, options.connect_port);
    if (!connect_port || *connect_port == 0) {
        runtime_.close_connect_listener();
        runtime_.close_pairing_listener();
        return false;
    }

    const bool pairing_mdns_published =
        runtime_.publish_pairing_mdns(options.bind_address, *pairing_port);
    if (!pairing_mdns_published) runtime_.remove_pairing_mdns();

    const bool connect_mdns_published =
        runtime_.publish_connect_mdns(options.bind_address, *connect_port);
    if (!connect_mdns_published) runtime_.remove_connect_mdns();

    bind_address_ = options.bind_address;
    pairing_port_ = *pairing_port;
    connect_port_ = *connect_port;
    pairing_listener_open_ = true;
    connect_listener_open_ = true;
    pairing_mdns_published_ = pairing_mdns_published;
    connect_mdns_published_ = connect_mdns_published;
    pairing_window_active_ = true;
    pairing_deadline_ = now + kPairingWindow;
    enabled_ = true;
    return true;
}

void WirelessControl::advance_time(Clock::time_point now) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!enabled_) return;

    if (!is_local_numeric_address(bind_address_)) {
        stop_locked();
        return;
    }

    if (pairing_window_active_ && now >= pairing_deadline_) close_pairing_locked();
}

bool WirelessControl::pairing_succeeded(std::string_view peer_id,
                                        std::string_view host_public_key) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!enabled_ || !pairing_window_active_ || !pairing_listener_open_) return false;
    if (!store_peer_locked(peer_id, host_public_key)) return false;
    close_pairing_locked();
    return true;
}

void WirelessControl::stop() noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    stop_locked();
}

bool WirelessControl::revoke_peer(std::string_view peer_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    return revoke_peer_locked(peer_id);
}

bool WirelessControl::enabled() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return enabled_;
}

std::uint16_t WirelessControl::pairing_port() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return pairing_port_;
}

std::uint16_t WirelessControl::connect_port() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return connect_port_;
}

WirelessControl::Status WirelessControl::status() const {
    std::lock_guard<std::mutex> lock(mutex_);
    Status result;
    result.enabled = enabled_;
    result.pairing_listener_open = pairing_listener_open_;
    result.connect_listener_open = connect_listener_open_;
    result.pairing_mdns_published = pairing_mdns_published_;
    result.connect_mdns_published = connect_mdns_published_;
    result.bind_address = bind_address_;
    result.pairing_port = pairing_port_;
    result.connect_port = connect_port_;
    std::vector<PairedPeer> peers = read_all_peers_locked();
    result.paired_peer_ids.reserve(peers.size());
    for (PairedPeer& peer : peers) {
        result.paired_peer_ids.push_back(std::move(peer.id));
    }
    return result;
}

bool WirelessControl::has_peer(std::string_view peer_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::string public_key;
    return read_peer_locked(peer_id, public_key);
}

void WirelessControl::for_each_paired_peer(const PairedPeerCallback& callback) const {
    if (!callback) return;
    std::vector<PairedPeer> peers;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        peers = read_all_peers_locked();
    }
    for (const PairedPeer& peer : peers) {
        if (!callback(peer.id, peer.public_key)) break;
    }
}

std::vector<std::string> WirelessControl::paired_peer_ids() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<PairedPeer> peers = read_all_peers_locked();
    std::vector<std::string> ids;
    ids.reserve(peers.size());
    for (PairedPeer& peer : peers) ids.push_back(std::move(peer.id));
    return ids;
}

int WirelessControl::open_peer_directory() const noexcept {
    try {
        const std::filesystem::path normalized = peer_store_directory_.lexically_normal();
        if (normalized.empty()) return -1;

        ScopedFd current(::open(normalized.is_absolute() ? "/" : ".",
                                O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
        if (current.get() < 0) return -1;

        bool has_directory_component = false;
        for (const std::filesystem::path& component : normalized) {
            const std::string name = component.string();
            if (name.empty() || name.find('\0') != std::string::npos) return -1;
            if (name == "/" || name == ".") continue;
            if (name == "..") return -1;
            has_directory_component = true;

            int next = ::openat(current.get(), name.c_str(),
                                O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
            if (next < 0 && errno == ENOENT) {
                if (::mkdirat(current.get(), name.c_str(), 0700) < 0 && errno != EEXIST) return -1;
                next = ::openat(current.get(), name.c_str(),
                                O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
            }
            if (next < 0) return -1;
            current = ScopedFd(next);
        }

        struct stat status {};
        if (!has_directory_component || ::fstat(current.get(), &status) < 0 ||
            !S_ISDIR(status.st_mode) || status.st_uid != ::geteuid() ||
            (status.st_mode & 0077) != 0) {
            return -1;
        }
        return current.release();
    } catch (...) {
        return -1;
    }
}

bool WirelessControl::store_peer_locked(std::string_view peer_id,
                                        std::string_view public_key) const noexcept {
    if (!is_peer_id(peer_id) || public_key.empty() || public_key.size() > kMaximumPublicKeySize) {
        return false;
    }

    try {
        ScopedFd directory(open_peer_directory());
        if (directory.get() < 0) return false;
        const std::string id(peer_id);

        ScopedFd old_peer(::openat(directory.get(), id.c_str(),
                                   O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK));
        if (old_peer.get() >= 0) {
            if (!valid_private_peer_file(old_peer.get())) return false;
        } else if (errno != ENOENT) {
            return false;
        } else if (read_all_peers_locked().size() >= kMaximumPeerCount) {
            return false;
        }

        static std::atomic<std::uint64_t> next_temp_id{0};
        std::string temporary_name;
        ScopedFd temporary;
        for (int attempt = 0; attempt < 16 && temporary.get() < 0; ++attempt) {
            const std::uint64_t temp_id = next_temp_id.fetch_add(1, std::memory_order_relaxed);
            temporary_name = ".mtpadbd-peer-" + std::to_string(::getpid()) + "-" +
                             std::to_string(temp_id);
            const int fd = ::openat(directory.get(), temporary_name.c_str(),
                                    O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
            if (fd >= 0) {
                temporary = ScopedFd(fd);
            } else if (errno != EEXIST) {
                return false;
            }
        }
        if (temporary.get() < 0) return false;

        bool written = ::fchmod(temporary.get(), 0600) == 0 && write_all(temporary.get(), public_key) &&
                       ::fsync(temporary.get()) == 0;
        if (written) written = ::renameat(directory.get(), temporary_name.c_str(), directory.get(), id.c_str()) == 0;
        if (!written) {
            ::unlinkat(directory.get(), temporary_name.c_str(), 0);
            return false;
        }
        return ::fsync(directory.get()) == 0;
    } catch (...) {
        return false;
    }
}

bool WirelessControl::read_peer_locked(std::string_view peer_id,
                                       std::string& public_key) const noexcept {
    if (!is_peer_id(peer_id)) return false;
    try {
        ScopedFd directory(open_peer_directory());
        if (directory.get() < 0) return false;
        const std::string id(peer_id);
        ScopedFd peer(::openat(directory.get(), id.c_str(),
                               O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK));
        if (peer.get() < 0 || !valid_private_peer_file(peer.get())) return false;

        struct stat status {};
        if (::fstat(peer.get(), &status) < 0) return false;
        std::string contents;
        contents.reserve(static_cast<std::size_t>(status.st_size));
        std::array<char, 1024> buffer{};
        for (;;) {
            const ssize_t count = ::read(peer.get(), buffer.data(), buffer.size());
            if (count < 0) {
                if (errno == EINTR) continue;
                return false;
            }
            if (count == 0) break;
            if (contents.size() + static_cast<std::size_t>(count) > kMaximumPublicKeySize) return false;
            contents.append(buffer.data(), static_cast<std::size_t>(count));
        }
        if (contents.empty()) return false;
        public_key = std::move(contents);
        return true;
    } catch (...) {
        return false;
    }
}

std::vector<WirelessControl::PairedPeer> WirelessControl::read_all_peers_locked() const {
    std::vector<PairedPeer> peers;
    ScopedFd directory(open_peer_directory());
    if (directory.get() < 0) return peers;

    const int scan_fd = ::dup(directory.get());
    if (scan_fd < 0) return peers;
    DIR* entries = ::fdopendir(scan_fd);
    if (entries == nullptr) {
        ::close(scan_fd);
        return peers;
    }

    while (peers.size() < kMaximumPeerCount) {
        errno = 0;
        dirent* entry = ::readdir(entries);
        if (entry == nullptr) break;
        const std::string_view id(entry->d_name);
        if (!is_peer_id(id)) continue;
        std::string public_key;
        if (read_peer_locked(id, public_key)) peers.push_back({std::string(id), std::move(public_key)});
    }
    ::closedir(entries);
    return peers;
}

bool WirelessControl::revoke_peer_locked(std::string_view peer_id) const noexcept {
    if (!is_peer_id(peer_id)) return false;
    try {
        ScopedFd directory(open_peer_directory());
        if (directory.get() < 0) return false;
        const std::string id(peer_id);
        ScopedFd peer(::openat(directory.get(), id.c_str(),
                               O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK));
        if (peer.get() < 0 || !valid_private_peer_file(peer.get())) return false;
        if (::unlinkat(directory.get(), id.c_str(), 0) < 0) return false;
        return ::fsync(directory.get()) == 0;
    } catch (...) {
        return false;
    }
}

void WirelessControl::stop_locked() noexcept {
    runtime_.close_pairing_listener();
    runtime_.close_connect_listener();
    runtime_.remove_pairing_mdns();
    runtime_.remove_connect_mdns();
    enabled_ = false;
    pairing_listener_open_ = false;
    connect_listener_open_ = false;
    pairing_mdns_published_ = false;
    connect_mdns_published_ = false;
    pairing_window_active_ = false;
    bind_address_.clear();
    pairing_port_ = 0;
    connect_port_ = 0;
    pairing_deadline_ = Clock::time_point{};
}

void WirelessControl::close_pairing_locked() noexcept {
    if (pairing_listener_open_) runtime_.close_pairing_listener();
    if (pairing_mdns_published_) runtime_.remove_pairing_mdns();
    pairing_listener_open_ = false;
    pairing_mdns_published_ = false;
    pairing_window_active_ = false;
    pairing_port_ = 0;
}

}  // namespace mtpadb::mtpadbd
