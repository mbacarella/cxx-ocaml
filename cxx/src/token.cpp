#include "cppcaml/token.hpp"

#include <array>

namespace cppcaml {

namespace {
// C-style escape so any byte sequence renders on one line and decodes
// identically on the OCaml oracle side (see oracle/dump_tokens.ml).
void escape_into(std::string_view s, std::string& out) {
  static const char* hex = "0123456789abcdef";
  for (unsigned char b : s) {
    switch (b) {
      case '\\': out += "\\\\"; break;
      case '"': out += "\\\""; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (b >= 0x20 && b <= 0x7e) {
          out += static_cast<char>(b);
        } else {
          out += "\\x";
          out += hex[b >> 4];
          out += hex[b & 0xf];
        }
    }
  }
}
}  // namespace

void print_canonical(const Token& t, std::ostream& os) {
  std::string line(kind_name(t.kind));
  switch (t.kind) {
    case Kind::LIDENT:
    case Kind::UIDENT:
    case Kind::LABEL:
    case Kind::OPTLABEL:
    case Kind::INFIXOP0:
    case Kind::INFIXOP1:
    case Kind::INFIXOP2:
    case Kind::INFIXOP3:
    case Kind::INFIXOP4:
    case Kind::PREFIXOP:
    case Kind::HASHOP:
    case Kind::DOTOP:
    case Kind::LETOP:
    case Kind::ANDOP:
      line += ' ';
      escape_into(t.text, line);
      break;
    case Kind::INT:
    case Kind::FLOAT:
      line += ' ';
      escape_into(t.text, line);
      if (t.modifier) {
        line += ' ';
        line += *t.modifier;
      }
      break;
    case Kind::CHAR:
      line += ' ';
      line += std::to_string(t.char_code);
      break;
    case Kind::STRING:
      line += ' ';
      line += t.delim ? ('{' + *t.delim + '}') : std::string("-");
      line += ' ';
      escape_into(t.text, line);
      break;
    case Kind::QUOTED_STRING_EXPR:
    case Kind::QUOTED_STRING_ITEM:
      line += ' ';
      escape_into(t.ext_id, line);
      line += ' ';
      line += t.delim ? ('{' + *t.delim + '}') : std::string("-");
      line += ' ';
      escape_into(t.text, line);
      break;
    default:
      break;  // nullary
  }
  os << line << '\t' << t.start << '\t' << t.end << '\n';
}

}  // namespace cppcaml
