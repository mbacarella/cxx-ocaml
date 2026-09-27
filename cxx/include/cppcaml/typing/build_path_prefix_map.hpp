// Port of utils/build_path_prefix_map.mli: the BUILD_PATH_PREFIX_MAP
// specification's encoding, decoding and rewriting of path prefixes
// (reproducible builds: dune sets it so that absolute paths recorded in
// artifacts name the workspace, not the build machine's directories).
#pragma once

#include <optional>
#include <string>
#include <vector>

namespace cppcaml::typing::build_path_prefix_map {

using path = std::string;
using path_prefix = std::string;
using error_message = std::string;

// Ok of 'a | Error of error_message
template <class T>
struct Result {
  std::optional<T> ok;
  error_message error;
};

std::string encode_prefix(const path_prefix& str);
Result<path_prefix> decode_prefix(const std::string& str);

struct Pair {
  path_prefix target;
  path_prefix source;
};
std::string encode_pair(const Pair& p);
Result<Pair> decode_pair(const std::string& str);

using Map = std::vector<std::optional<Pair>>;  // pair option list
std::string encode_map(const Map& map);
Result<Map> decode_map(const std::string& str);

// rewrite_first: the last matching pair wins (the list is scanned reversed)
std::optional<path> rewrite_first(const Map& map, const path& p);
std::vector<path> rewrite_all(const Map& map, const path& p);
path rewrite(const Map& map, const path& p);
std::vector<path> invert_all(const Map& map, const path& p);

}  // namespace cppcaml::typing::build_path_prefix_map
