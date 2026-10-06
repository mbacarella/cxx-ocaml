// The target's command-line options (Arch.command_line_options) and the
// configurations its port supports, for the native driver.
#pragma once

#include <utility>
#include <vector>

#include "cppcaml/typing/arg.hpp"

namespace cppcaml::typing::arch {
std::vector<arg::Option> command_line_options();
// The Config values the ported code generator was verified for (it reads
// them as constants): (variable, value)
const std::vector<std::pair<const char*, const char*>>& supported_configuration();
}  // namespace cppcaml::typing::arch
