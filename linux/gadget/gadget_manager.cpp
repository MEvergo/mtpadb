#include "gadget_manager.h"

#include "functionfs_descriptors.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <exception>
#include <fcntl.h>
#include <filesystem>
#include <iostream>
#include <mntent.h>
#include <optional>
#include <pthread.h>
#include <signal.h>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <sys/file.h>
#include <sys/mount.h>
#include <sys/signalfd.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace mtpadb::gadget {
namespace {

namespace fs = std::filesystem;

constexpr char kConfigFsRoot[] = "/sys/kernel/config";
constexpr char kGadgetName[] = "mtpadb";
constexpr char kFfsMount[] = "/dev/ffs-mtpadb";
constexpr char kFfsInstance[] = "mtpadb";
constexpr char kUdcRoot[] = "/sys/class/udc";
constexpr char kLockPath[] = "/run/lock/mtpadb-gadgetd.lock";

const fs::path kGadgetPath = fs::path(kConfigFsRoot) / "usb_gadget" / kGadgetName;

class UniqueFd {
  public:
    explicit UniqueFd(int fd = -1) noexcept : fd_(fd) {}
    ~UniqueFd() {
        if (fd_ >= 0) close(fd_);
    }
    UniqueFd(const UniqueFd&) = delete;
    UniqueFd& operator=(const UniqueFd&) = delete;
    UniqueFd(UniqueFd&& other) noexcept : fd_(other.fd_) { other.fd_ = -1; }
    UniqueFd& operator=(UniqueFd&& other) noexcept {
        if (this == &other) return *this;
        if (fd_ >= 0) close(fd_);
        fd_ = other.fd_;
        other.fd_ = -1;
        return *this;
    }
    int get() const noexcept { return fd_; }

  private:
    int fd_;
};

[[noreturn]] void throw_errno(std::string_view action) {
    throw std::system_error(errno, std::generic_category(), std::string(action));
}

struct MountInfo {
    std::string source;
    std::string target;
    std::string type;
};

std::optional<MountInfo> mount_at(std::string_view target) {
    FILE* mounts = setmntent("/proc/self/mounts", "r");
    if (mounts == nullptr) throw_errno("open /proc/self/mounts");

    std::optional<MountInfo> result;
    while (mntent* entry = getmntent(mounts)) {
        if (target == entry->mnt_dir) {
            result = MountInfo{entry->mnt_fsname, entry->mnt_dir, entry->mnt_type};
            break;
        }
    }
    endmntent(mounts);
    return result;
}

void ensure_configfs() {
    const auto mounted = mount_at(kConfigFsRoot);
    if (mounted) {
        if (mounted->type != "configfs") {
            throw std::runtime_error("/sys/kernel/config is mounted with a non-configfs filesystem");
        }
        return;
    }

    std::error_code error;
    fs::create_directories(kConfigFsRoot, error);
    if (error) throw std::system_error(error, "create /sys/kernel/config");
    if (mount("configfs", kConfigFsRoot, "configfs", 0, nullptr) < 0) {
        throw_errno("mount configfs at /sys/kernel/config");
    }
}

bool path_entry_exists(const fs::path& path) {
    struct stat status {};
    if (lstat(path.c_str(), &status) == 0) return true;
    if (errno == ENOENT) return false;
    throw_errno(std::string("inspect ") + path.string());
}

void make_configfs_directory(const fs::path& path) {
    if (mkdir(path.c_str(), 0755) < 0) throw_errno(std::string("mkdir ") + path.string());
}

void remove_directory_if_present(const fs::path& path) {
    if (rmdir(path.c_str()) == 0 || errno == ENOENT) return;
    throw_errno(std::string("rmdir ") + path.string());
}

void remove_link_if_present(const fs::path& path) {
    if (unlink(path.c_str()) == 0 || errno == ENOENT) return;
    throw_errno(std::string("unlink ") + path.string());
}

void write_once(int fd, const void* data, std::size_t size, std::string_view action) {
    ssize_t written;
    do {
        written = write(fd, data, size);
    } while (written < 0 && errno == EINTR);
    if (written < 0) throw_errno(action);
    if (static_cast<std::size_t>(written) != size) {
        throw std::runtime_error(std::string(action) + ": short write");
    }
}

void write_attribute(const fs::path& path, std::string_view text) {
    UniqueFd fd(open(path.c_str(), O_WRONLY | O_CLOEXEC));
    if (fd.get() < 0) throw_errno(std::string("open ") + path.string());
    std::string value(text);
    value.push_back('\n');
    write_once(fd.get(), value.data(), value.size(), std::string("write ") + path.string());
}

std::optional<std::string> read_attribute(const fs::path& path) {
    UniqueFd fd(open(path.c_str(), O_RDONLY | O_CLOEXEC));
    if (fd.get() < 0) {
        if (errno == ENOENT) return std::nullopt;
        throw_errno(std::string("open ") + path.string());
    }

    std::string value;
    std::array<char, 128> buffer{};
    for (;;) {
        ssize_t count;
        do {
            count = read(fd.get(), buffer.data(), buffer.size());
        } while (count < 0 && errno == EINTR);
        if (count < 0) throw_errno(std::string("read ") + path.string());
        if (count == 0) break;
        value.append(buffer.data(), static_cast<std::size_t>(count));
    }
    if (!value.empty() && value.back() == '\n') value.pop_back();
    return value;
}

bool is_dummy_udc(std::string_view name) {
    if (name.empty() || name == "." || name == ".." || name.find('/') != std::string_view::npos) {
        return false;
    }
    std::error_code error;
    const fs::path driver = fs::canonical(fs::path(kUdcRoot) / name / "device" / "driver", error);
    return !error && driver.filename() == "dummy_udc";
}

std::string select_udc(const GadgetConfig& config) {
    if (config.udc) {
        if (!is_dummy_udc(*config.udc)) {
            throw std::runtime_error("configured UDC is not backed by dummy_hcd; refusing to expose ADB on a physical controller");
        }
        return *config.udc;
    }

    std::error_code error;
    std::vector<std::string> candidates;
    for (fs::directory_iterator it(kUdcRoot, error), end; !error && it != end; it.increment(error)) {
        const std::string name = it->path().filename().string();
        if (is_dummy_udc(name)) candidates.push_back(name);
    }
    if (error) throw std::system_error(error, "enumerate /sys/class/udc");
    if (candidates.empty()) {
        throw std::runtime_error("no dummy_hcd UDC found; load dummy_hcd (physical UDCs are intentionally rejected)");
    }
    std::sort(candidates.begin(), candidates.end());
    return candidates.front();
}

std::string format_id(std::uint16_t value) {
    char buffer[7];
    std::snprintf(buffer, sizeof(buffer), "0x%04x", value);
    return buffer;
}

void create_gadget(const GadgetConfig& config) {
    make_configfs_directory(kGadgetPath);
    write_attribute(kGadgetPath / "idVendor", format_id(config.vid));
    write_attribute(kGadgetPath / "idProduct", format_id(config.pid));
    write_attribute(kGadgetPath / "bcdUSB", "0x0200");
    write_attribute(kGadgetPath / "bcdDevice", "0x0100");
    write_attribute(kGadgetPath / "bDeviceClass", "0x00");
    write_attribute(kGadgetPath / "bDeviceSubClass", "0x00");
    write_attribute(kGadgetPath / "bDeviceProtocol", "0x00");
    write_attribute(kGadgetPath / "bMaxPacketSize0", "64");

    make_configfs_directory(kGadgetPath / "strings" / "0x409");
    write_attribute(kGadgetPath / "strings" / "0x409" / "manufacturer", config.manufacturer);
    write_attribute(kGadgetPath / "strings" / "0x409" / "product", config.product);
    write_attribute(kGadgetPath / "strings" / "0x409" / "serialnumber", config.serial);

    make_configfs_directory(kGadgetPath / "configs" / "c.1");
    write_attribute(kGadgetPath / "configs" / "c.1" / "MaxPower", "100");
    write_attribute(kGadgetPath / "configs" / "c.1" / "bmAttributes", "0x80");
    make_configfs_directory(kGadgetPath / "configs" / "c.1" / "strings" / "0x409");
    write_attribute(kGadgetPath / "configs" / "c.1" / "strings" / "0x409" / "configuration", "MTPADB ADB");
    make_configfs_directory(kGadgetPath / "functions" / "ffs.mtpadb");
}

void ensure_functionfs_mount() {
    const auto mounted = mount_at(kFfsMount);
    if (mounted) {
        if (mounted->type != "functionfs" || mounted->source != kFfsInstance) {
            throw std::runtime_error("/dev/ffs-mtpadb is mounted by a different filesystem or FunctionFS instance");
        }
        return;
    }

    std::error_code error;
    fs::create_directories(kFfsMount, error);
    if (error) throw std::system_error(error, "create /dev/ffs-mtpadb");
    if (mount(kFfsInstance, kFfsMount, "functionfs", 0, nullptr) < 0) {
        throw_errno("mount FunctionFS instance mtpadb");
    }
}

void write_functionfs_blob(int ep0, std::span<const std::uint8_t> blob, std::string_view label) {
    write_once(ep0, blob.data(), blob.size(), label);
}

void link_gadget_function() {
    const fs::path link = kGadgetPath / "configs" / "c.1" / "ffs.mtpadb";
    const fs::path function = kGadgetPath / "functions" / "ffs.mtpadb";
    if (symlink(function.c_str(), link.c_str()) < 0) throw_errno("link FunctionFS gadget function");
}

void bind_gadget(std::string_view udc) {
    write_attribute(kGadgetPath / "UDC", udc);
}

void setup_and_wait(const GadgetConfig& config, int signal_fd) {
    const std::string udc = select_udc(config);
    create_gadget(config);
    ensure_functionfs_mount();

    UniqueFd ep0(open((fs::path(kFfsMount) / "ep0").c_str(), O_RDWR | O_CLOEXEC));
    if (ep0.get() < 0) throw_errno("open FunctionFS ep0");
    write_functionfs_blob(ep0.get(), build_functionfs_descriptors(), "write FunctionFS descriptors");
    write_functionfs_blob(ep0.get(), build_functionfs_strings(), "write FunctionFS strings");

    link_gadget_function();
    UniqueFd ep_out(open((fs::path(kFfsMount) / "ep1").c_str(), O_RDWR | O_CLOEXEC));
    if (ep_out.get() < 0) throw_errno("open FunctionFS ADB bulk OUT ep1");
    UniqueFd ep_in(open((fs::path(kFfsMount) / "ep2").c_str(), O_RDWR | O_CLOEXEC));
    if (ep_in.get() < 0) throw_errno("open FunctionFS ADB bulk IN ep2");
    bind_gadget(udc);

    std::cout << "MTPADB gadget bound to dummy UDC " << udc
              << "; ADB protocol remains offline until Stage B\n" << std::flush;
    signalfd_siginfo info{};
    for (;;) {
        const ssize_t count = read(signal_fd, &info, sizeof(info));
        if (count == static_cast<ssize_t>(sizeof(info))) return;
        if (count < 0 && errno == EINTR) continue;
        if (count < 0) throw_errno("read termination signal");
        throw std::runtime_error("short read from signalfd");
    }
}

class ProcessLock {
  public:
    ProcessLock() : fd_(open(kLockPath, O_CREAT | O_RDWR | O_CLOEXEC, 0600)) {
        if (fd_.get() < 0) throw_errno("open mtpadb gadget lock");
        if (flock(fd_.get(), LOCK_EX | LOCK_NB) < 0) {
            if (errno == EWOULDBLOCK) throw std::runtime_error("another mtpadb-gadgetd process owns the gadget");
            throw_errno("lock mtpadb gadget");
        }
    }

  private:
    UniqueFd fd_;
};

void cleanup_unlocked() {
    const auto configfs_mount = mount_at(kConfigFsRoot);
    if (configfs_mount && configfs_mount->type != "configfs") {
        throw std::runtime_error("/sys/kernel/config is not configfs");
    }

    if (configfs_mount && path_entry_exists(kGadgetPath)) {
        const fs::path udc_attribute = kGadgetPath / "UDC";
        if (const auto bound = read_attribute(udc_attribute); bound && !bound->empty()) {
            if (!is_dummy_udc(*bound)) {
                throw std::runtime_error("refusing to unbind mtpadb gadget from a non-dummy UDC");
            }
            write_attribute(udc_attribute, "");
        }
        remove_link_if_present(kGadgetPath / "configs" / "c.1" / "ffs.mtpadb");
    }

    if (const auto mounted = mount_at(kFfsMount)) {
        if (mounted->type != "functionfs" || mounted->source != kFfsInstance) {
            throw std::runtime_error("refusing to unmount a foreign filesystem at /dev/ffs-mtpadb");
        }
        if (umount2(kFfsMount, 0) < 0) throw_errno("unmount FunctionFS mtpadb");
    }

    if (configfs_mount && path_entry_exists(kGadgetPath)) {
        remove_directory_if_present(kGadgetPath / "functions" / "ffs.mtpadb");
        remove_directory_if_present(kGadgetPath / "configs" / "c.1" / "strings" / "0x409");
        remove_directory_if_present(kGadgetPath / "configs" / "c.1");
        remove_directory_if_present(kGadgetPath / "strings" / "0x409");
        remove_directory_if_present(kGadgetPath);
    }

    if (!mount_at(kFfsMount)) remove_directory_if_present(kFfsMount);
}

void require_root() {
    if (geteuid() != 0) throw std::runtime_error("mtpadb-gadgetd must run as root");
}

int make_signal_fd() {
    sigset_t signals;
    sigemptyset(&signals);
    sigaddset(&signals, SIGINT);
    sigaddset(&signals, SIGTERM);
    const int result = pthread_sigmask(SIG_BLOCK, &signals, nullptr);
    if (result != 0) throw std::system_error(result, std::generic_category(), "block termination signals");
    const int fd = signalfd(-1, &signals, SFD_CLOEXEC);
    if (fd < 0) throw_errno("create signalfd");
    return fd;
}

}  // namespace

void run_gadget(const GadgetConfig& config) {
    require_root();
    ProcessLock lock;
    ensure_configfs();
    cleanup_unlocked();
    UniqueFd signal_fd(make_signal_fd());
    try {
        setup_and_wait(config, signal_fd.get());
    } catch (...) {
        const auto failure = std::current_exception();
        try {
            cleanup_unlocked();
        } catch (const std::exception& cleanup_error) {
            std::cerr << "mtpadb-gadgetd cleanup after failure: " << cleanup_error.what() << '\n';
        }
        std::rethrow_exception(failure);
    }
    cleanup_unlocked();
}

void cleanup_gadget() {
    require_root();
    ProcessLock lock;
    cleanup_unlocked();
}

}  // namespace mtpadb::gadget
