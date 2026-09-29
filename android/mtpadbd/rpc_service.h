#pragma once

namespace mtpadb::mtpadbd {

int connect_mtprpcd_socket(const char* path) noexcept;
int connect_mtprpcd_socket() noexcept;

}  // namespace mtpadb::mtpadbd
