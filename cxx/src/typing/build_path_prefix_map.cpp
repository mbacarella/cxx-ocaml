// Port of utils/build_path_prefix_map.ml.
#include "cppcaml/typing/build_path_prefix_map.hpp"

#include <cstdio>

namespace cppcaml::typing::build_path_prefix_map {

namespace {
template <class T>
Result<T> errorf(std::string msg) {
  Result<T> r;
  r.error = std::move(msg);
  return r;
}
template <class T>
Result<T> ok(T v) {
  Result<T> r;
  r.ok = std::move(v);
  return r;
}
// %S: OCaml's String.escaped between quotes
std::string quoted(const std::string& s) {
  std::string r = "\"";
  for (unsigned char c : s) {
    switch (c) {
      case '"': r += "\\\""; break;
      case '\\': r += "\\\\"; break;
      case '\n': r += "\\n"; break;
      case '\t': r += "\\t"; break;
      case '\r': r += "\\r"; break;
      case '\b': r += "\\b"; break;
      default:
        if (c >= ' ' && c <= '~') r += static_cast<char>(c);
        else {
          char buf[5];
          std::snprintf(buf, sizeof buf, "\\%03u", c);
          r += buf;
        }
    }
  }
  return r + "\"";
}
}  // namespace

std::string encode_prefix(const path_prefix& str) {
  std::string buf;
  for (char c : str) {
    switch (c) {
      case '%': buf += "%#"; break;
      case '=': buf += "%+"; break;
      case ':': buf += "%."; break;
      default: buf += c;
    }
  }
  return buf;
}

Result<path_prefix> decode_prefix(const std::string& str) {
  std::string buf;
  std::size_t i = 0;
  while (i < str.size()) {
    char c = str[i];
    if (c == '=' || c == ':') return errorf<path_prefix>(std::string("invalid character '") + c + "' in key or value");
    if (c == '%') {
      if (i + 1 == str.size()) return errorf<path_prefix>("invalid encoded string " + quoted(str) + " (trailing '%')");
      switch (str[i + 1]) {
        case '#': buf += '%'; break;
        case '+': buf += '='; break;
        case '.': buf += ':'; break;
        default: return errorf<path_prefix>(std::string("invalid %-escaped character '") + str[i + 1] + "'");
      }
      i += 2;
    } else {
      buf += c;
      i += 1;
    }
  }
  return ok(buf);
}

std::string encode_pair(const Pair& p) { return encode_prefix(p.target) + "=" + encode_prefix(p.source); }

Result<Pair> decode_pair(const std::string& str) {
  std::size_t equal_pos = str.find('=');
  if (equal_pos == std::string::npos)
    return errorf<Pair>("invalid key/value pair " + quoted(str) + ", no '=' separator");
  std::string encoded_target = str.substr(0, equal_pos);
  std::string encoded_source = str.substr(equal_pos + 1);
  // match decode_prefix encoded_target, decode_prefix encoded_source: the
  // tuple's components are evaluated right to left, the target's error wins
  Result<path_prefix> s = decode_prefix(encoded_source);
  Result<path_prefix> t = decode_prefix(encoded_target);
  if (t.ok && s.ok) return ok(Pair{*t.ok, *s.ok});
  if (!t.ok) return errorf<Pair>(t.error);
  return errorf<Pair>(s.error);
}

std::string encode_map(const Map& map) {
  std::string r;
  for (std::size_t i = 0; i < map.size(); ++i) {
    if (i > 0) r += ':';
    if (map[i]) r += encode_pair(*map[i]);
  }
  return r;
}

Result<Map> decode_map(const std::string& str) {
  // String.split_on_char ':' then List.map (left to right; the first
  // invalid pair's error)
  std::vector<std::string> pairs;
  std::size_t start = 0;
  for (std::size_t i = 0; i <= str.size(); ++i)
    if (i == str.size() || str[i] == ':') {
      pairs.push_back(str.substr(start, i - start));
      start = i + 1;
    }
  Map map;
  for (const std::string& pair : pairs) {
    if (pair.empty()) {
      map.push_back(std::nullopt);
      continue;
    }
    Result<Pair> p = decode_pair(pair);
    if (!p.ok) return errorf<Map>(p.error);
    map.push_back(*p.ok);
  }
  return ok(map);
}

namespace {
std::optional<path> make_target(const path& p, const std::optional<Pair>& e) {
  if (!e) return std::nullopt;
  const Pair& pr = *e;
  bool is_prefix = pr.source.size() <= p.size() && p.compare(0, pr.source.size(), pr.source) == 0;
  if (is_prefix) return pr.target + p.substr(pr.source.size());
  return std::nullopt;
}
std::optional<path> make_source(const path& p, const std::optional<Pair>& e) {
  if (!e) return std::nullopt;
  const Pair& pr = *e;
  if (p.size() >= pr.target.size() && p.compare(0, pr.target.size(), pr.target) == 0)
    return pr.source + p.substr(pr.target.size());
  return std::nullopt;
}
}  // namespace

std::optional<path> rewrite_first(const Map& map, const path& p) {
  for (auto it = map.rbegin(); it != map.rend(); ++it)
    if (auto r = make_target(p, *it)) return r;
  return std::nullopt;
}

std::vector<path> rewrite_all(const Map& map, const path& p) {
  std::vector<path> r;
  for (auto it = map.rbegin(); it != map.rend(); ++it)
    if (auto t = make_target(p, *it)) r.push_back(*t);
  return r;
}

path rewrite(const Map& map, const path& p) {
  std::optional<path> r = rewrite_first(map, p);
  return r ? *r : p;
}

std::vector<path> invert_all(const Map& map, const path& p) {
  std::vector<path> r;
  for (auto it = map.rbegin(); it != map.rend(); ++it)
    if (auto s = make_source(p, *it)) r.push_back(*s);
  return r;
}

}  // namespace cppcaml::typing::build_path_prefix_map
