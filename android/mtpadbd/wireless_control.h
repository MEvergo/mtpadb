#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace mtpadb::mtpadbd {

class WirelessRuntime {
  public:
    virtual ~WirelessRuntime() = default;

    virtual std::optional<std::uint16_t> open_pairing_listener(std::string_view bind_address,
                                                               std::uint16_t requested_port,
                                                               std::string_view pairing_code) = 0;
    virtual std::optional<std::uint16_t> open_connect_listener(std::string_view bind_address,
                                                               std::uint16_t requested_port) = 0;
    virtual void close_pairing_listener() noexcept = 0;
    virtual void close_connect_listener() noexcept = 0;

    virtual bool publish_pairing_mdns(std::string_view bind_address, std::uint16_t port) = 0;
    virtual bool publish_connect_mdns(std::string_view bind_address, std::uint16_t port) = 0;
    virtual void remove_pairing_mdns() noexcept = 0;
    virtual void remove_connect_mdns() noexcept = 0;
};

class WirelessControl {
  public:
    using Clock = std::chrono::steady_clock;

    struct Options {
        std::string bind_address;
        std::uint16_t pairing_port = 0;
        std::uint16_t connect_port = 0;
        std::string pairing_code;
    };
    struct Status {
        bool enabled = false;
        bool pairing_listener_open = false;
        bool connect_listener_open = false;
        bool pairing_mdns_published = false;
        bool connect_mdns_published = false;
        std::string bind_address;
        std::uint16_t pairing_port = 0;
        std::uint16_t connect_port = 0;
        std::vector<std::string> paired_peer_ids;
    };

    // Returning false from the callback stops iteration.
    using PairedPeerCallback = std::function<bool(std::string_view peer_id,
                                                  std::string_view public_key)>;

    explicit WirelessControl(
        WirelessRuntime& runtime,
        std::filesystem::path peer_store_directory = "/data/misc/adb/mtpadb/wireless");
    ~WirelessControl();

    WirelessControl(const WirelessControl&) = delete;
    WirelessControl& operator=(const WirelessControl&) = delete;

    bool start(const Options& options, Clock::time_point now);
    void advance_time(Clock::time_point now);
    bool pairing_succeeded(std::string_view peer_id, std::string_view host_public_key);
    void stop() noexcept;
    bool revoke_peer(std::string_view peer_id);

    bool enabled() const noexcept;
    std::uint16_t pairing_port() const noexcept;
    std::uint16_t connect_port() const noexcept;
    Status status() const;
    bool has_peer(std::string_view peer_id) const;

    // The callback receives copies whose views remain valid for the duration of each call.
    // It is invoked without the control's state/storage lock held.
    void for_each_paired_peer(const PairedPeerCallback& callback) const;

    // Safe for public status: these identifiers never contain stored public-key material.
    std::vector<std::string> paired_peer_ids() const;

  private:
    struct PairedPeer {
        std::string id;
        std::string public_key;
    };

    int open_peer_directory() const noexcept;
    bool store_peer_locked(std::string_view peer_id, std::string_view public_key) const noexcept;
    bool read_peer_locked(std::string_view peer_id, std::string& public_key) const noexcept;
    std::vector<PairedPeer> read_all_peers_locked() const;
    bool revoke_peer_locked(std::string_view peer_id) const noexcept;
    void stop_locked() noexcept;
    void close_pairing_locked() noexcept;

    WirelessRuntime& runtime_;
    const std::filesystem::path peer_store_directory_;

    mutable std::mutex mutex_;
    bool enabled_ = false;
    bool pairing_listener_open_ = false;
    bool connect_listener_open_ = false;
    bool pairing_mdns_published_ = false;
    bool connect_mdns_published_ = false;
    bool pairing_window_active_ = false;
    std::string bind_address_;
    std::uint16_t pairing_port_ = 0;
    std::uint16_t connect_port_ = 0;
    Clock::time_point pairing_deadline_{};
};

}  // namespace mtpadb::mtpadbd
