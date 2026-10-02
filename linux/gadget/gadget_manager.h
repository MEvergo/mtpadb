#pragma once

#include "gadget_config.h"

namespace mtpadb::gadget {

void run_gadget(const GadgetConfig& config);
void cleanup_gadget();

}  // namespace mtpadb::gadget
