// Port of parsing/lexer.mll. See lexer.hpp for the public interface.
//
// Strategy: a hand-written scanner that mirrors the ocamllex rules. ocamllex
// uses maximal-munch with ties broken by rule order; we reproduce that for the
// tricky operator cases by computing both the longest "dedicated" spelling and
// the longest open-ended operator run, then taking the longer (ties -> the
// dedicated/earlier rule).
#include "cppcaml/lexer.hpp"

#include <array>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace cppcaml {

namespace {

inline bool is_blank(char c) { return c == ' ' || c == '\t' || c == '\f'; }
inline bool is_lower(char c) { return (c >= 'a' && c <= 'z') || c == '_'; }
inline bool is_upper(char c) { return c >= 'A' && c <= 'Z'; }
inline bool is_digit(char c) { return c >= '0' && c <= '9'; }
inline bool is_identchar(char c) {
  return is_digit(c) || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
         c == '_' || c == '\'';
}
inline bool is_hex(char c) {
  return is_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}
inline bool is_symbolchar(char c) {
  switch (c) {
    case '!': case '$': case '%': case '&': case '*': case '+': case '-':
    case '.': case '/': case ':': case '<': case '=': case '>': case '?':
    case '@': case '^': case '|': case '~':
      return true;
    default:
      return false;
  }
}
inline bool is_dotsymbolchar(char c) {
  // symbolchar minus '.'
  return c != '.' && is_symbolchar(c);
}
inline bool is_symbolchar_or_hash(char c) { return c == '#' || is_symbolchar(c); }
inline bool is_kwdopchar(char c) {
  switch (c) {
    case '$': case '&': case '*': case '+': case '-': case '/':
    case '<': case '=': case '>': case '@': case '^': case '|':
      return true;
    default:
      return false;
  }
}

int digit_value(char c) {
  if (c >= 'a' && c <= 'f') return 10 + c - 'a';
  if (c >= 'A' && c <= 'F') return 10 + c - 'A';
  return c - '0';
}

char char_for_backslash(char c) {
  switch (c) {
    case 'n': return '\n';
    case 'r': return '\r';
    case 'b': return '\b';
    case 't': return '\t';
    default: return c;
  }
}

// --- UTF-8 + Latin-9 identifier support (ports utils/misc.ml Utf8_lexeme) ---
//
// OCaml only admits Latin-9 letters (plus ASCII) in identifiers, via the fixed
// table get_known_char. We port exactly that bounded set.

enum class L9 { None, Upper, Lower };
L9 latin9_case(int cp) {
  if ((cp >= 0xc0 && cp <= 0xd6) || (cp >= 0xd8 && cp <= 0xde) ||
      cp == 0x160 || cp == 0x17d || cp == 0x152 || cp == 0x178 || cp == 0x1e9e)
    return L9::Upper;
  if ((cp >= 0xe0 && cp <= 0xf6) || (cp >= 0xf8 && cp <= 0xfe) ||
      cp == 0xdf || cp == 0xff || cp == 0x161 || cp == 0x17e || cp == 0x153)
    return L9::Lower;
  return L9::None;
}
bool uchar_is_uppercase(int cp) {
  if (cp < 0x80) return cp >= 'A' && cp <= 'Z';
  return latin9_case(cp) == L9::Upper;
}
bool uchar_valid_in_identifier(int cp, bool with_dot = false) {
  if (cp < 0x80)
    return (cp >= 'a' && cp <= 'z') || (cp >= 'A' && cp <= 'Z') ||
           (cp >= '0' && cp <= '9') || cp == '_' || cp == '\'' ||
           (with_dot && cp == '.');
  return latin9_case(cp) != L9::None;
}
bool uchar_not_identifier_start(int cp) {
  return (cp >= '0' && cp <= '9') || cp == '\'';
}

// Decode one UTF-8 scalar at s[i]. On a malformed lead/continuation, returns the
// replacement scalar 0xFFFD with length 1 (mirrors Uchar.rep in validation).
struct UDec { int cp; size_t len; };
UDec utf8_decode(std::string_view s, size_t i) {
  auto b = [&](size_t k) { return k < s.size() ? static_cast<unsigned char>(s[k]) : 0; };
  unsigned c0 = b(i);
  auto cont = [&](size_t k) { return b(k) >= 0x80 && b(k) <= 0xBF; };
  if (c0 < 0x80) return {static_cast<int>(c0), 1};
  if (c0 >= 0xC2 && c0 <= 0xDF && cont(i + 1))
    return {static_cast<int>(((c0 & 0x1F) << 6) | (b(i + 1) & 0x3F)), 2};
  if (c0 >= 0xE0 && c0 <= 0xEF && cont(i + 1) && cont(i + 2))
    return {static_cast<int>(((c0 & 0x0F) << 12) | ((b(i + 1) & 0x3F) << 6) |
                             (b(i + 2) & 0x3F)),
            3};
  if (c0 >= 0xF0 && c0 <= 0xF4 && cont(i + 1) && cont(i + 2) && cont(i + 3))
    return {static_cast<int>(((c0 & 0x07) << 18) | ((b(i + 1) & 0x3F) << 12) |
                             ((b(i + 2) & 0x3F) << 6) | (b(i + 3) & 0x3F)),
            4};
  return {0xFFFD, 1};  // invalid
}

void append_utf8(std::string& out, int cp) {
  if (cp < 0x80) {
    out += static_cast<char>(cp);
  } else if (cp < 0x800) {
    out += static_cast<char>(0xC0 | (cp >> 6));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  } else if (cp < 0x10000) {
    out += static_cast<char>(0xE0 | (cp >> 12));
    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  } else {
    out += static_cast<char>(0xF0 | (cp >> 18));
    out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  }
}

bool is_scalar_value(int cp) {
  return cp >= 0 && cp <= 0x10FFFF && !(cp >= 0xD800 && cp <= 0xDFFF);
}

// NFD->NFC for the Latin-9 base+combining pairs (ports get_known_pair).
int known_pair(char base, int comb) {
  switch (comb) {
    case 0x300:  // grave
      switch (base) { case 'A': return 0xc0; case 'E': return 0xc8; case 'I': return 0xcc;
        case 'O': return 0xd2; case 'U': return 0xd9; case 'a': return 0xe0; case 'e': return 0xe8;
        case 'i': return 0xec; case 'o': return 0xf2; case 'u': return 0xf9; } break;
    case 0x301:  // acute
      switch (base) { case 'A': return 0xc1; case 'E': return 0xc9; case 'I': return 0xcd;
        case 'O': return 0xd3; case 'U': return 0xda; case 'Y': return 0xdd; case 'a': return 0xe1;
        case 'e': return 0xe9; case 'i': return 0xed; case 'o': return 0xf3; case 'u': return 0xfa;
        case 'y': return 0xfd; } break;
    case 0x302:  // circumflex
      switch (base) { case 'A': return 0xc2; case 'E': return 0xca; case 'I': return 0xce;
        case 'O': return 0xd4; case 'U': return 0xdb; case 'a': return 0xe2; case 'e': return 0xea;
        case 'i': return 0xee; case 'o': return 0xf4; case 'u': return 0xfb; } break;
    case 0x303:  // tilde
      switch (base) { case 'A': return 0xc3; case 'N': return 0xd1; case 'O': return 0xd5;
        case 'a': return 0xe3; case 'n': return 0xf1; case 'o': return 0xf5; } break;
    case 0x308:  // diaeresis
      switch (base) { case 'A': return 0xc4; case 'E': return 0xcb; case 'I': return 0xcf;
        case 'O': return 0xd6; case 'U': return 0xdc; case 'Y': return 0x178; case 'a': return 0xe4;
        case 'e': return 0xeb; case 'i': return 0xef; case 'o': return 0xf6; case 'u': return 0xfc;
        case 'y': return 0xff; } break;
    case 0x30a:  // ring
      switch (base) { case 'A': return 0xc5; case 'a': return 0xe5; } break;
    case 0x327:  // cedilla
      switch (base) { case 'C': return 0xc7; case 'c': return 0xe7; } break;
    case 0x30c:  // caron
      switch (base) { case 'S': return 0x160; case 'Z': return 0x17d; case 's': return 0x161;
        case 'z': return 0x17e; } break;
  }
  return -1;
}

// Normalize a UTF-8 string to NFC over the Latin-9 pairs. Returns (nfc, valid),
// valid == false if any scalar is malformed/non-scalar (ports normalize).
std::pair<std::string, bool> utf8_normalize(std::string_view s) {
  if (s.empty()) return {"", true};
  std::string out;
  UDec d = utf8_decode(s, 0);
  bool valid = d.cp != 0xFFFD && is_scalar_value(d.cp);
  int prev = d.cp;
  size_t i = d.len;
  while (i < s.size()) {
    UDec dn = utf8_decode(s, i);
    valid = valid && dn.cp != 0xFFFD && is_scalar_value(dn.cp);
    int comb = (prev >= 0 && prev < 256) ? known_pair(static_cast<char>(prev), dn.cp) : -1;
    if (comb >= 0) {
      prev = comb;
    } else {
      append_utf8(out, prev);
      prev = dn.cp;
    }
    i += dn.len;
  }
  append_utf8(out, prev);
  return {out, valid};
}

// Whether a normalized identifier string is all-lowercase (ports is_lowercase).
bool ident_is_lowercase(std::string_view s) {
  for (size_t i = 0; i < s.size();) {
    UDec d = utf8_decode(s, i);
    if (!uchar_valid_in_identifier(d.cp) || uchar_is_uppercase(d.cp)) return false;
    i += d.len;
  }
  return true;
}

// The OCaml keyword table (lexer.mll all_keywords). Only the default edition is
// modelled: every keyword whose "since" version is <= the language version is
// active. For trunk all of these are active. Value tokens (lor/mod/...) map to
// INFIXOPn with the keyword as text.
struct KwEntry { Kind kind; const char* text; };
const std::unordered_map<std::string_view, KwEntry>& keyword_table() {
  static const std::unordered_map<std::string_view, KwEntry> t = {
      {"and", {Kind::AND, nullptr}},
      {"as", {Kind::AS, nullptr}},
      {"assert", {Kind::ASSERT, nullptr}},
      {"begin", {Kind::BEGIN, nullptr}},
      {"class", {Kind::CLASS, nullptr}},
      {"constraint", {Kind::CONSTRAINT, nullptr}},
      {"do", {Kind::DO, nullptr}},
      {"done", {Kind::DONE, nullptr}},
      {"downto", {Kind::DOWNTO, nullptr}},
      {"effect", {Kind::EFFECT, nullptr}},
      {"else", {Kind::ELSE, nullptr}},
      {"end", {Kind::END, nullptr}},
      {"exception", {Kind::EXCEPTION, nullptr}},
      {"external", {Kind::EXTERNAL, nullptr}},
      {"false", {Kind::FALSE, nullptr}},
      {"for", {Kind::FOR, nullptr}},
      {"fun", {Kind::FUN, nullptr}},
      {"function", {Kind::FUNCTION, nullptr}},
      {"functor", {Kind::FUNCTOR, nullptr}},
      {"if", {Kind::IF, nullptr}},
      {"in", {Kind::IN, nullptr}},
      {"include", {Kind::INCLUDE, nullptr}},
      {"inherit", {Kind::INHERIT, nullptr}},
      {"initializer", {Kind::INITIALIZER, nullptr}},
      {"lazy", {Kind::LAZY, nullptr}},
      {"let", {Kind::LET, nullptr}},
      {"match", {Kind::MATCH, nullptr}},
      {"method", {Kind::METHOD, nullptr}},
      {"module", {Kind::MODULE, nullptr}},
      {"mutable", {Kind::MUTABLE, nullptr}},
      {"new", {Kind::NEW, nullptr}},
      {"nonrec", {Kind::NONREC, nullptr}},
      {"object", {Kind::OBJECT, nullptr}},
      {"of", {Kind::OF, nullptr}},
      {"open", {Kind::OPEN, nullptr}},
      {"or", {Kind::OR, nullptr}},
      {"private", {Kind::PRIVATE, nullptr}},
      {"rec", {Kind::REC, nullptr}},
      {"sig", {Kind::SIG, nullptr}},
      {"struct", {Kind::STRUCT, nullptr}},
      {"then", {Kind::THEN, nullptr}},
      {"to", {Kind::TO, nullptr}},
      {"true", {Kind::TRUE, nullptr}},
      {"try", {Kind::TRY, nullptr}},
      {"type", {Kind::TYPE, nullptr}},
      {"val", {Kind::VAL, nullptr}},
      {"virtual", {Kind::VIRTUAL, nullptr}},
      {"when", {Kind::WHEN, nullptr}},
      {"while", {Kind::WHILE, nullptr}},
      {"with", {Kind::WITH, nullptr}},
      {"lor", {Kind::INFIXOP3, "lor"}},
      {"lxor", {Kind::INFIXOP3, "lxor"}},
      {"mod", {Kind::INFIXOP3, "mod"}},
      {"land", {Kind::INFIXOP3, "land"}},
      {"lsl", {Kind::INFIXOP4, "lsl"}},
      {"lsr", {Kind::INFIXOP4, "lsr"}},
      {"asr", {Kind::INFIXOP4, "asr"}},
  };
  return t;
}

// Dedicated symbolic spellings (closed tokens made of symbol/bracket chars).
// Ordered longest-first at lookup time. Maps spelling -> Kind (INFIXOP entries
// carry their own text == spelling).
const std::vector<std::pair<std::string_view, Kind>>& dedicated_symbols() {
  static const std::vector<std::pair<std::string_view, Kind>> v = {
      {"!=", Kind::INFIXOP0},
      {"->", Kind::MINUSGREATER}, {"-.", Kind::MINUSDOT},
      {"+.", Kind::PLUSDOT}, {"+=", Kind::PLUSEQ},
      {"::", Kind::COLONCOLON}, {":=", Kind::COLONEQUAL}, {":>", Kind::COLONGREATER},
      {"<-", Kind::LESSMINUS}, {"&&", Kind::AMPERAMPER},
      {"||", Kind::BARBAR}, {"|]", Kind::BARRBRACKET},
      {">]", Kind::GREATERRBRACKET}, {">}", Kind::GREATERRBRACE},
      {"..", Kind::DOTDOT},
      {"!", Kind::BANG}, {"+", Kind::PLUS}, {"-", Kind::MINUS},
      {"*", Kind::STAR}, {"=", Kind::EQUAL}, {"<", Kind::LESS}, {">", Kind::GREATER},
      {"|", Kind::BAR}, {"&", Kind::AMPERSAND}, {"%", Kind::PERCENT},
      {".", Kind::DOT}, {":", Kind::COLON},
  };
  return v;
}

}  // namespace

bool is_ocaml_keyword(std::string_view s) {
  auto it = keyword_table().find(s);
  return it != keyword_table().end();
}

Token Lexer::raw_token() {
  // Skip blanks and escaped newlines (which produce no token), per the
  // `blank+` and `'\\' newline` rules.
  for (;;) {
    while (!eof() && is_blank(cur())) pos_++;
    if (cur() == '\\' && (at(pos_ + 1) == '\n' || at(pos_ + 1) == '\r')) {
      pos_++;  // backslash
      while (cur() == '\r') pos_++;
      if (cur() == '\n') pos_++;
      continue;
    }
    break;
  }

  size_t start = pos_;
  if (eof()) return Token::make(Kind::TEOF, start, start);

  char c = cur();

  // newline -> EOL  (newline = '\r'* '\n')
  if (c == '\n' || c == '\r') {
    size_t p = pos_;
    while (at(p) == '\r') p++;
    if (at(p) == '\n') {
      pos_ = p + 1;
      return Token::make(Kind::EOL, start, pos_);
    }
    // lone '\r' (not part of a newline): skip it, try again. (TODO: OCaml
    // treats this as an illegal character; rare in practice.)
    pos_++;
    return raw_token();
  }

  // raw identifier escape: "\#" before an identifier lets a keyword be used as
  // a (lowercase) identifier.
  if (c == '\\' && at(pos_ + 1) == '#') return scan_ident(start);
  if (is_lower(c) || is_upper(c) || static_cast<unsigned char>(c) >= 0xC0)
    return scan_ident(start);
  if (is_digit(c)) return scan_number(start);
  if (c == '"') return scan_string(start);
  if (c == '\'') return scan_char_or_quote(start);
  if (c == '(') {
    if (at(pos_ + 1) == '*') {
      scan_comment();
      if (is_doc_comment(start, pos_)) {
        Token t = Token::make(Kind::DOCSTRING, start, pos_);
        t.text = doc_body(start, pos_);
        return t;
      }
      Token t = Token::make(Kind::COMMENT, start, pos_);
      return t;  // filtered out by next()
    }
    pos_++;
    return Token::make(Kind::LPAREN, start, pos_);
  }
  if (c == ')') { pos_++; return Token::make(Kind::RPAREN, start, pos_); }
  if (c == '~') return scan_label_or_tilde(start);
  if (c == '?') return scan_optlabel_or_question(start);
  if (c == '{') return scan_brace(start);
  if (c == '}') { pos_++; return Token::make(Kind::RBRACE, start, pos_); }
  if (c == '[') {
    // longest dedicated among [@@@ [@@ [@ [%% [% [| [< [> [
    static const std::pair<std::string_view, Kind> br[] = {
        {"[@@@", Kind::LBRACKETATATAT}, {"[@@", Kind::LBRACKETATAT},
        {"[%%", Kind::LBRACKETPERCENTPERCENT}, {"[@", Kind::LBRACKETAT},
        {"[%", Kind::LBRACKETPERCENT}, {"[|", Kind::LBRACKETBAR},
        {"[<", Kind::LBRACKETLESS}, {"[>", Kind::LBRACKETGREATER},
        {"[", Kind::LBRACKET},
    };
    for (auto& [sp, k] : br) {
      if (looking_at(sp)) { pos_ += sp.size(); return Token::make(k, start, pos_); }
    }
  }
  if (c == ']') { pos_++; return Token::make(Kind::RBRACKET, start, pos_); }
  if (c == '#') return scan_hash(start);
  if (c == ',') { pos_++; return Token::make(Kind::COMMA, start, pos_); }
  if (c == ';') {
    if (at(pos_ + 1) == ';') { pos_ += 2; return Token::make(Kind::SEMISEMI, start, pos_); }
    pos_++;
    return Token::make(Kind::SEMI, start, pos_);
  }
  if (c == '`') { pos_++; return Token::make(Kind::BACKQUOTE, start, pos_); }

  if (is_symbolchar(c)) return scan_symbol(start);

  throw LexError("Illegal character", start);
}

Token Lexer::next() {
  for (;;) {
    Token t = raw_token();
    if (t.kind == Kind::COMMENT || t.kind == Kind::EOL ||
        t.kind == Kind::DOCSTRING)
      continue;
    return t;
  }
}

// Is the comment src_[s..e) a docstring?  `(**` (but not `(***…`), or `(**)`.
bool Lexer::is_doc_comment(size_t s, size_t e) const {
  if (e - s < 4) return false;  // shortest doc is "(**)"
  if (!(src_[s] == '(' && src_[s + 1] == '*' && src_[s + 2] == '*')) return false;
  if (e - s == 4 && src_[s + 3] == ')') return true;  // (**) empty docstring
  return src_[s + 3] != '*';                           // (***… is a plain comment
}
std::string Lexer::doc_body(size_t s, size_t e) const {
  // content between the leading "(**" and trailing "*)"
  size_t b = s + 3, en = e >= 2 ? e - 2 : s;
  return en > b ? std::string(src_.substr(b, en - b)) : std::string();
}

std::vector<Token> Lexer::tokenize() {
  // Ports lexer.mll's `token`/`attach` state machine: between consecutive real
  // tokens, accumulate comments/EOLs/docstrings and route the docstrings into the
  // pre/post/floating/extra tables (see DocAttach).
  enum Lines { NoLine, NewLine, BlankLine };
  // doc_state: Initial | After(a) | Before(a,f,b); lists are most-recent-first.
  struct DS { int tag = 0; std::vector<Docstring> a, f, b; };  // 0=Initial 1=After 2=Before
  auto rev = [](std::vector<Docstring> v) { std::reverse(v.begin(), v.end()); return v; };

  std::vector<Token> out;
  out.reserve(src_.size() / 4 + 16);  // ~avg token length; avoids realloc-move churn
  size_t prev_end = 0;  // post_pos: end of the previous real token
  for (;;) {
    Lines lines = NoLine;
    DS docs;
    Token tok;
    for (;;) {
      Token rt = raw_token();
      if (rt.kind == Kind::COMMENT) {
        if (lines == NewLine) lines = NoLine;  // NoLine/BlankLine unchanged
        continue;
      }
      if (rt.kind == Kind::EOL) {
        lines = lines == NoLine ? NewLine : BlankLine;
        continue;
      }
      if (rt.kind == Kind::DOCSTRING) {
        Docstring d{rt.text, rt.start, rt.end};
        bool blank = lines == BlankLine;
        if (docs.tag == 0) {                       // Initial
          if (!blank) { docs.tag = 1; docs.a = {d}; }
          else { docs.tag = 2; docs.b = {d}; }
        } else if (docs.tag == 1) {                // After(a)
          if (!blank) docs.a.insert(docs.a.begin(), d);
          else { docs.tag = 2; docs.b = {d}; /* f stays [] */ }
        } else {                                   // Before(a,f,b)
          if (!blank) docs.b.insert(docs.b.begin(), d);
          else {  // f := b @ f ; b := [d]
            std::vector<Docstring> nf = docs.b;
            nf.insert(nf.end(), docs.f.begin(), docs.f.end());
            docs.f = std::move(nf);
            docs.b = {d};
          }
        }
        lines = NoLine;
        continue;
      }
      tok = std::move(rt);  // a real token: attach the accumulated docs and stop
      break;
    }
    size_t pre_pos = tok.start;
    bool blank = lines == BlankLine;
    auto put = [&](std::unordered_map<size_t, std::vector<Docstring>>& m, size_t k,
                   std::vector<Docstring> v) { if (!v.empty()) m[k] = std::move(v); };
    if (docs.tag == 1) {  // After(a)
      put(docs_.post, prev_end, rev(docs.a));
      if (!blank) put(docs_.pre, pre_pos, docs.a);
      else put(docs_.pre_extra, pre_pos, rev(docs.a));
    } else if (docs.tag == 2) {  // Before(a,f,b)
      put(docs_.post, prev_end, rev(docs.a));
      std::vector<Docstring> fb = rev(docs.f);  // rev_append f (rev b) = rev f ++ rev b
      { auto rb = rev(docs.b); fb.insert(fb.end(), rb.begin(), rb.end()); }
      put(docs_.post_extra, prev_end, fb);
      put(docs_.pre_extra, pre_pos, rev(docs.a));
      if (!blank) { put(docs_.floating, pre_pos, rev(docs.f)); put(docs_.pre, pre_pos, docs.b); }
      else put(docs_.floating, pre_pos, fb);
    }
    Kind k = tok.kind;
    prev_end = tok.end;
    out.push_back(std::move(tok));
    if (k == Kind::TEOF) break;
  }
  return out;
}

Token Lexer::scan_ident(size_t start) {
  // Optional raw-identifier escape "\#": forces a lowercase identifier and
  // bypasses the keyword table (e.g. \#and -> LIDENT "and"). The escape is part
  // of the lexeme span but not of the identifier text.
  bool raw_escape = false;
  size_t name_start = start;
  if (src_[pos_] == '\\' && at(pos_ + 1) == '#') {
    raw_escape = true;
    pos_ += 2;
    name_start = pos_;
  }

  // Consume identstart_ext identchar_ext* by byte pattern (matches the ocamllex
  // regexp): a utf8 "char" is a lead byte 0xC0-0xFF + continuation 0x80-0xBF*.
  bool has_utf8 = false;
  auto consume_utf8 = [&](size_t p) -> size_t {
    size_t q = p + 1;
    while (q < src_.size() && static_cast<unsigned char>(src_[q]) >= 0x80 &&
           static_cast<unsigned char>(src_[q]) <= 0xBF)
      q++;
    return q;
  };
  size_t p = pos_;
  if (static_cast<unsigned char>(src_[p]) >= 0xC0) { has_utf8 = true; p = consume_utf8(p); }
  else p++;  // ascii letter or '_'
  while (p < src_.size()) {
    unsigned char ch = src_[p];
    if (is_identchar(static_cast<char>(ch))) p++;
    else if (ch >= 0xC0) { has_utf8 = true; p = consume_utf8(p); }
    else break;
  }
  pos_ = p;
  std::string name(src_.substr(name_start, p - name_start));

  if (!has_utf8) {
    char c0 = name[0];
    bool capitalized = (c0 >= 'A' && c0 <= 'Z');
    if (raw_escape) {
      if (capitalized) throw LexError("Capitalized raw identifier", start);
      Token t = Token::make(Kind::LIDENT, start, pos_);
      t.text = std::move(name);
      return t;
    }
    if (capitalized) {
      Token t = Token::make(Kind::UIDENT, start, pos_);
      t.text = std::move(name);
      return t;
    }
    if (name == "_") return Token::make(Kind::UNDERSCORE, start, pos_);
    // let-op / and-op: "let"/"and" immediately followed by kwdopchar.
    if ((name == "let" || name == "and") && is_kwdopchar(cur())) {
      pos_++;  // kwdopchar
      while (!eof() && is_dotsymbolchar(cur())) pos_++;
      Token t = Token::make(name == "let" ? Kind::LETOP : Kind::ANDOP, start, pos_);
      t.text = std::string(src_.substr(start, pos_ - start));
      return t;
    }
    auto it = keyword_table().find(name);
    if (it != keyword_table().end()) {
      Token t = Token::make(it->second.kind, start, pos_);
      if (it->second.text) t.text = it->second.text;  // INFIXOP keyword (mod, lsl, ...)
      return t;
    }
    Token t = Token::make(Kind::LIDENT, start, pos_);
    t.text = std::move(name);
    return t;
  }

  // Extended (contains Latin-9 / utf8): normalize to NFC, validate, classify by
  // capitalization of the first scalar.
  auto [nfc, enc_ok] = utf8_normalize(name);
  if (!enc_ok) throw LexError("Invalid encoding of identifier", name_start);
  for (size_t i = 0; i < nfc.size();) {
    UDec d = utf8_decode(nfc, i);
    if (!uchar_valid_in_identifier(d.cp))
      throw LexError("Invalid char in identifier", name_start);
    if (i == 0 && uchar_not_identifier_start(d.cp))
      throw LexError("Invalid ident start", name_start);
    i += d.len;
  }
  UDec first = utf8_decode(nfc, 0);
  bool capitalized = uchar_is_uppercase(first.cp);
  if (raw_escape && capitalized) throw LexError("Capitalized raw identifier", start);
  Kind k = (!raw_escape && capitalized) ? Kind::UIDENT : Kind::LIDENT;
  Token t = Token::make(k, start, pos_);
  t.text = std::move(nfc);
  return t;
}

Token Lexer::scan_number(size_t start) {
  bool is_float = false;
  auto eat_run = [&](auto pred) {
    while (!eof() && (pred(cur()) || cur() == '_')) pos_++;
  };
  if (cur() == '0' && (at(pos_ + 1) == 'x' || at(pos_ + 1) == 'X')) {
    pos_ += 2;
    eat_run(is_hex);
    if (cur() == '.') { is_float = true; pos_++; eat_run(is_hex); }
    if (cur() == 'p' || cur() == 'P') {
      is_float = true; pos_++;
      if (cur() == '+' || cur() == '-') pos_++;
      eat_run(is_digit);
    }
  } else if (cur() == '0' && (at(pos_ + 1) == 'o' || at(pos_ + 1) == 'O')) {
    pos_ += 2;
    eat_run([](char c) { return c >= '0' && c <= '7'; });
  } else if (cur() == '0' && (at(pos_ + 1) == 'b' || at(pos_ + 1) == 'B')) {
    pos_ += 2;
    eat_run([](char c) { return c == '0' || c == '1'; });
  } else {
    eat_run(is_digit);
    if (cur() == '.') { is_float = true; pos_++; eat_run(is_digit); }
    if (cur() == 'e' || cur() == 'E') {
      is_float = true; pos_++;
      if (cur() == '+' || cur() == '-') pos_++;
      eat_run(is_digit);
    }
  }
  std::optional<char> modifier;
  if ((cur() >= 'G' && cur() <= 'Z') || (cur() >= 'g' && cur() <= 'z')) {
    modifier = cur();
    pos_++;
  }
  // Invalid literal: a number immediately glued to identifier chars.
  if (!modifier && !eof() && is_identchar(cur())) {
    while (!eof() && is_identchar(cur())) pos_++;
    throw LexError("Invalid literal", start);
  }
  Token t = Token::make(is_float ? Kind::FLOAT : Kind::INT, start, pos_);
  // Literal text excludes the modifier (matches lexer.mll's `lit`).
  size_t lit_end = modifier ? pos_ - 1 : pos_;
  t.text = std::string(src_.substr(start, lit_end - start));
  t.modifier = modifier;
  return t;
}

Token Lexer::scan_char_or_quote(size_t start) {
  // Try the char-literal patterns (all end with a closing quote). If none
  // applies, it's a bare QUOTE (e.g. a type variable 'a).
  size_t p = pos_ + 1;  // after opening '
  auto closing_at = [&](size_t q) { return at(q) == '\''; };

  // '\n' literal:  ' \r*\n '
  {
    size_t q = p;
    while (at(q) == '\r') q++;
    if (at(q) == '\n' && closing_at(q + 1)) {
      pos_ = q + 2;
      Token t = Token::make(Kind::CHAR, start, pos_);
      t.char_code = '\n';
      return t;
    }
  }
  char c0 = at(p);
  if (c0 == '\\') {
    char e = at(p + 1);
    if ((e == '\\' || e == '\'' || e == '"' || e == 'n' || e == 't' ||
         e == 'b' || e == 'r' || e == ' ') && closing_at(p + 2)) {
      pos_ = p + 3;
      Token t = Token::make(Kind::CHAR, start, pos_);
      t.char_code = static_cast<unsigned char>(char_for_backslash(e));
      return t;
    }
    if (is_digit(e) && is_digit(at(p + 2)) && is_digit(at(p + 3)) && closing_at(p + 4)) {
      int v = 100 * (e - '0') + 10 * (at(p + 2) - '0') + (at(p + 3) - '0');
      pos_ = p + 5;
      Token t = Token::make(Kind::CHAR, start, pos_);
      t.char_code = v;
      return t;
    }
    if (e == 'o' && at(p + 2) >= '0' && at(p + 2) <= '7' && at(p + 3) >= '0' &&
        at(p + 3) <= '7' && at(p + 4) >= '0' && at(p + 4) <= '7' && closing_at(p + 5)) {
      int v = 64 * (at(p + 2) - '0') + 8 * (at(p + 3) - '0') + (at(p + 4) - '0');
      pos_ = p + 6;
      Token t = Token::make(Kind::CHAR, start, pos_);
      t.char_code = v;
      return t;
    }
    if (e == 'x' && is_hex(at(p + 2)) && is_hex(at(p + 3)) && closing_at(p + 4)) {
      int v = 16 * digit_value(at(p + 2)) + digit_value(at(p + 3));
      pos_ = p + 5;
      Token t = Token::make(Kind::CHAR, start, pos_);
      t.char_code = v;
      return t;
    }
  } else if (c0 != '\'' && c0 != '\n' && c0 != '\r' && closing_at(p + 1)) {
    // ' <char> '
    pos_ = p + 2;
    Token t = Token::make(Kind::CHAR, start, pos_);
    t.char_code = static_cast<unsigned char>(c0);
    return t;
  }
  // '' is an empty character literal error in OCaml.
  if (c0 == '\'') throw LexError("Empty character literal", start);

  // Bare quote.
  pos_++;
  return Token::make(Kind::QUOTE, start, pos_);
}

Token Lexer::scan_string(size_t start) {
  pos_++;  // opening "
  strbuf_.clear();
  for (;;) {
    if (eof()) throw LexError("Unterminated string", start);
    char c = cur();
    if (c == '"') { pos_++; break; }
    if (c == '\\') {
      char e = at(pos_ + 1);
      if (e == '\n' || e == '\r') {
        // line continuation: backslash, newline, blanks -> nothing stored
        pos_++;  // backslash
        while (cur() == '\r') pos_++;
        if (cur() == '\n') pos_++;
        while (is_blank(cur())) pos_++;
        continue;
      }
      if (e == '\\' || e == '\'' || e == '"' || e == 'n' || e == 't' ||
          e == 'b' || e == 'r' || e == ' ') {
        store(char_for_backslash(e));
        pos_ += 2;
        continue;
      }
      if (is_digit(e) && is_digit(at(pos_ + 2)) && is_digit(at(pos_ + 3))) {
        int v = 100 * (e - '0') + 10 * (at(pos_ + 2) - '0') + (at(pos_ + 3) - '0');
        if (v > 255) throw LexError("Illegal decimal escape", pos_);  // out of 0-255
        store(static_cast<char>(v));
        pos_ += 4;
        continue;
      }
      if (e == 'o' && at(pos_ + 2) >= '0' && at(pos_ + 2) <= '7' &&
          at(pos_ + 3) >= '0' && at(pos_ + 3) <= '7' && at(pos_ + 4) >= '0' &&
          at(pos_ + 4) <= '7') {
        int v = 64 * (at(pos_ + 2) - '0') + 8 * (at(pos_ + 3) - '0') + (at(pos_ + 4) - '0');
        if (v > 255) throw LexError("Illegal octal escape", pos_);  // out of 0-255
        store(static_cast<char>(v));
        pos_ += 5;
        continue;
      }
      if (e == 'x' && is_hex(at(pos_ + 2)) && is_hex(at(pos_ + 3))) {
        store(static_cast<char>(16 * digit_value(at(pos_ + 2)) + digit_value(at(pos_ + 3))));
        pos_ += 4;  // '\' 'x' H H
        continue;
      }
      if (e == 'u' && at(pos_ + 2) == '{' && is_hex(at(pos_ + 3))) {
        // \u{HHH} -> UTF-8 encode the scalar value (1..6 hex digits).
        size_t q = pos_ + 3;
        int cp = 0, ndig = 0;
        while (is_hex(at(q))) { cp = cp * 16 + digit_value(at(q)); q++; ndig++; }
        if (ndig > 6 || !is_scalar_value(cp)) throw LexError("Illegal \\u escape", pos_);
        if (at(q) == '}') q++;
        append_utf8(strbuf_, cp);
        pos_ = q;
        continue;
      }
      // '\\' followed by anything else: lax, store the backslash + char raw.
      store('\\');
      store(e);
      pos_ += 2;
      continue;
    }
    if (c == '\n' || c == '\r') {
      // #12502: normalize \r*\n by dropping only the *first* carriage return;
      // any further CRs and the \n are kept verbatim (\r\r\n -> \r\n).
      size_t q = pos_;
      while (at(q) == '\r') q++;
      if (at(q) == '\n') {
        size_t from = (src_[pos_] == '\r') ? pos_ + 1 : pos_;
        store(src_.substr(from, (q + 1) - from));
        pos_ = q + 1;
      } else {
        store(c);
        pos_++;
      }
      continue;
    }
    store(c);
    pos_++;
  }
  Token t = Token::make(Kind::STRING, start, pos_);
  t.text = strbuf_;
  t.delim = std::nullopt;
  return t;
}

Token Lexer::scan_quoted_string(size_t start, const std::string& delim) {
  // pos_ is just past the opening {delim|. Content runs until |delim}.
  strbuf_.clear();
  std::string closer = "|" + delim + "}";
  for (;;) {
    if (eof()) throw LexError("Unterminated string", start);
    if (looking_at(closer)) {
      pos_ += closer.size();
      break;
    }
    char c = cur();
    if (c == '\n' || c == '\r') {
      size_t q = pos_;
      while (at(q) == '\r') q++;
      if (at(q) == '\n') {
        size_t from = (src_[pos_] == '\r') ? pos_ + 1 : pos_;
        store(src_.substr(from, (q + 1) - from));
        pos_ = q + 1;
      } else { store(c); pos_++; }
      continue;
    }
    store(c);
    pos_++;
  }
  Token t = Token::make(Kind::STRING, start, pos_);
  t.text = strbuf_;
  t.delim = delim;
  return t;
}

void Lexer::scan_comment() {
  // pos_ at "(*". Consume balanced, string-aware, through matching "*)".
  pos_ += 2;
  int depth = 1;
  while (depth > 0) {
    if (eof()) throw LexError("Unterminated comment", 0);
    if (looking_at("(*")) { depth++; pos_ += 2; continue; }
    if (looking_at("*)")) { depth--; pos_ += 2; continue; }
    if (cur() == '\'') {
      // A quote immediately after an identifier character is an identifier prime
      // (`f'`), not a char-literal opener -- so `(* f' '"' *)` is not mis-lexed
      // (treating the `'` as a char start would swallow the following `"..."` as
      // a string and run off the end).  OCaml's lexer makes the same distinction.
      if (pos_ > 0 && is_identchar(src_[pos_ - 1])) { pos_++; continue; }
      // Skip a char literal so that a quoted double-quote (e.g. '"') inside the
      // comment doesn't spuriously start a string.  Mirrors OCaml's comment
      // rule: only the genuine char-literal shapes are consumed; a bare quote
      // (a type variable like 'a) just advances one char.
      size_t p = pos_ + 1;  // after opening '
      auto closes = [&](size_t q) { return at(q) == '\''; };
      size_t end = 0;  // one past the closing quote, or 0 if not a char literal
      // ' \r* \n '
      { size_t q = p; while (at(q) == '\r') q++;
        if (at(q) == '\n' && closes(q + 1)) end = q + 2; }
      if (!end) {
        char c0 = at(p);
        if (c0 == '\\') {
          char e = at(p + 1);
          if ((e == '\\' || e == '\'' || e == '"' || e == 'n' || e == 't' ||
               e == 'b' || e == 'r' || e == ' ') && closes(p + 2)) end = p + 3;
          else if (is_digit(e) && is_digit(at(p + 2)) && is_digit(at(p + 3)) &&
                   closes(p + 4)) end = p + 5;
          else if (e == 'o' && at(p + 2) >= '0' && at(p + 2) <= '7' &&
                   at(p + 3) >= '0' && at(p + 3) <= '7' && at(p + 4) >= '0' &&
                   at(p + 4) <= '7' && closes(p + 5)) end = p + 6;
          else if (e == 'x' && is_hex(at(p + 2)) && is_hex(at(p + 3)) &&
                   closes(p + 4)) end = p + 5;
        } else if (c0 != '\'' && c0 != '\n' && c0 != '\r' && closes(p + 1)) {
          end = p + 2;
        }
      }
      if (end) pos_ = end; else pos_++;  // char literal, or bare quote
      continue;
    }
    if (cur() == '"') {
      // skip a string literal so that "*)" inside it doesn't end the comment
      pos_++;
      while (!eof() && cur() != '"') {
        if (cur() == '\\' && !eof()) { pos_ += 2; continue; }
        pos_++;
      }
      if (!eof()) pos_++;  // closing "
      continue;
    }
    if (cur() == '{') {
      // skip a quoted string {delim|...|delim} (incl. {%id|, {%%id|) so that
      // "*)" inside it doesn't end the comment. Uses lax delimiter validation.
      size_t p = pos_ + 1;
      if (at(p) == '%') { p++; if (at(p) == '%') p++;
        while (p < src_.size() && (is_identchar(src_[p]) || src_[p] == '.' ||
                                   static_cast<unsigned char>(src_[p]) >= 0x80)) p++;
        while (p < src_.size() && is_blank(src_[p])) p++;
      }
      size_t d0 = p;
      while (p < src_.size() && ((src_[p] >= 'a' && src_[p] <= 'z') ||
                                 (src_[p] >= 'A' && src_[p] <= 'Z') ||
                                 static_cast<unsigned char>(src_[p]) >= 0x80)) p++;
      if (p < src_.size() && src_[p] == '|') {
        auto [delim, ok] = utf8_normalize(src_.substr(d0, p - d0));
        if (ok && ident_is_lowercase(delim)) {
          pos_ = p + 1;
          std::string closer = "|" + delim + "}";
          while (!eof() && !looking_at(closer)) pos_++;
          if (!eof()) pos_ += closer.size();
          continue;
        }
      }
      pos_++;  // not a quoted string: '{' is an ordinary comment character
      continue;
    }
    pos_++;
  }
}

Token Lexer::scan_label_or_tilde(size_t start) {
  // '~' (lowercase identchar*) ':' -> LABEL ;  '~' symbolchar_or_hash+ -> PREFIXOP ; else TILDE
  size_t p = pos_ + 1;
  if (is_lower(p < src_.size() ? src_[p] : '\0') && src_[p] != '_') {
    // tentatively read identstart identchar*
    size_t q = p + 1;
    while (q < src_.size() && is_identchar(src_[q])) q++;
    if (q < src_.size() && src_[q] == ':') {
      Token t = Token::make(Kind::LABEL, start, q + 1);
      t.text = std::string(src_.substr(p, q - p));
      pos_ = q + 1;
      return t;
    }
  }
  if (is_symbolchar_or_hash(at(pos_ + 1))) {
    size_t q = pos_ + 1;
    while (q < src_.size() && is_symbolchar_or_hash(src_[q])) q++;
    Token t = Token::make(Kind::PREFIXOP, start, q);
    t.text = std::string(src_.substr(start, q - start));
    pos_ = q;
    return t;
  }
  pos_++;
  return Token::make(Kind::TILDE, start, pos_);
}

Token Lexer::scan_optlabel_or_question(size_t start) {
  // '?' (lowercase identchar*) ':' -> OPTLABEL ;  '?' symbolchar_or_hash+ -> PREFIXOP ; else QUESTION
  size_t p = pos_ + 1;
  if (p < src_.size() && ((src_[p] >= 'a' && src_[p] <= 'z') || src_[p] == '_')) {
    size_t q = p + 1;
    while (q < src_.size() && is_identchar(src_[q])) q++;
    if (q < src_.size() && src_[q] == ':') {
      Token t = Token::make(Kind::OPTLABEL, start, q + 1);
      t.text = std::string(src_.substr(p, q - p));
      pos_ = q + 1;
      return t;
    }
  }
  if (is_symbolchar_or_hash(at(pos_ + 1))) {
    size_t q = pos_ + 1;
    while (q < src_.size() && is_symbolchar_or_hash(src_[q])) q++;
    Token t = Token::make(Kind::PREFIXOP, start, q);
    t.text = std::string(src_.substr(start, q - start));
    pos_ = q;
    return t;
  }
  pos_++;
  return Token::make(Kind::QUESTION, start, pos_);
}

Token Lexer::scan_brace(size_t start) {
  auto is_delim_byte = [](unsigned char ch) {
    // delim_ext = (lowercase | uppercase | utf8)* ; any high byte is utf8.
    return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || ch >= 0x80;
  };

  // Quoted-string extensions:
  //   {%  extattrident [blank+ delim] |  ...  |delim}  -> QUOTED_STRING_EXPR
  //   {%% extattrident [blank+ delim] |  ...  |delim}  -> QUOTED_STRING_ITEM
  if (looking_at("{%")) {
    bool item = looking_at("{%%");
    size_t p = pos_ + (item ? 3 : 2);
    size_t id_start = p;
    // extattrident = ident_ext ('.' ident_ext)*
    while (p < src_.size()) {
      unsigned char ch = src_[p];
      if (is_identchar(static_cast<char>(ch)) || ch == '.' || ch >= 0x80) p++;
      else break;
    }
    std::string id(src_.substr(id_start, p - id_start));
    std::string delim;
    bool ok = false;
    if (p < src_.size() && src_[p] == '|') { p++; ok = true; }  // empty delim
    else {
      while (p < src_.size() && is_blank(src_[p])) p++;
      size_t d0 = p;
      while (p < src_.size() && is_delim_byte(src_[p])) p++;
      delim = std::string(src_.substr(d0, p - d0));
      if (p < src_.size() && src_[p] == '|') { p++; ok = true; }
    }
    if (ok) {
      pos_ = p;  // just past the opening '|'
      Token t = scan_quoted_string(start, delim);
      t.kind = item ? Kind::QUOTED_STRING_ITEM : Kind::QUOTED_STRING_EXPR;
      t.ext_id = std::move(id);
      t.delim = delim;  // Some delim (Some "" when empty)
      t.content_start = p;  // byte offset of the content (just past `|`)
      return t;
    }
    // malformed extension head: fall back to LBRACE
    pos_++;
    return Token::make(Kind::LBRACE, start, pos_);
  }

  // "{" delim_ext "|"  -> quoted string. The delimiter must normalize to a
  // lowercase identifier (validate_delim), else it is an error.
  size_t p = pos_ + 1;
  while (p < src_.size() && is_delim_byte(src_[p])) p++;
  if (p < src_.size() && src_[p] == '|') {
    auto [delim, enc_ok] = utf8_normalize(src_.substr(pos_ + 1, p - (pos_ + 1)));
    if (!enc_ok) throw LexError("Invalid encoding of delimiter", start);
    if (!ident_is_lowercase(delim)) throw LexError("Non-lowercase delimiter", start);
    pos_ = p + 1;  // past '|'
    return scan_quoted_string(start, delim);
  }
  if (at(pos_ + 1) == '<') { pos_ += 2; return Token::make(Kind::LBRACELESS, start, pos_); }
  pos_++;
  return Token::make(Kind::LBRACE, start, pos_);
}

Token Lexer::scan_hash(size_t start) {
  // Line directive: at the beginning of a line,
  //   # [ \t]* <num> [ \t]* "<file>" [^\n\r]*
  // updates location and produces no token. An out-of-range <num> is an error.
  bool at_bol = (start == 0) || (src_[start - 1] == '\n');
  if (at_bol) {
    size_t p = pos_ + 1;
    while (p < src_.size() && (src_[p] == ' ' || src_[p] == '\t')) p++;
    size_t num_start = p;
    while (p < src_.size() && is_digit(src_[p])) p++;
    if (p > num_start) {
      size_t num_end = p;
      while (p < src_.size() && (src_[p] == ' ' || src_[p] == '\t')) p++;
      if (p < src_.size() && src_[p] == '"') {
        size_t q = p + 1;
        while (q < src_.size() && src_[q] != '"' && src_[q] != '\n' && src_[q] != '\r') q++;
        if (q < src_.size() && src_[q] == '"') {
          // matched a directive head; consume the rest of the line
          size_t r = q + 1;
          while (r < src_.size() && src_[r] != '\n' && src_[r] != '\r') r++;
          // OCaml parses <num> with int_of_string (63-bit); overflow -> error.
          std::string_view num = src_.substr(num_start, num_end - num_start);
          size_t nz = num.find_first_not_of('0');
          std::string_view sig = (nz == std::string_view::npos) ? "0" : num.substr(nz);
          static constexpr std::string_view kMaxInt = "4611686018427387903";  // 2^62-1
          bool overflow = sig.size() > kMaxInt.size() ||
                          (sig.size() == kMaxInt.size() && sig > kMaxInt);
          if (overflow) throw LexError("Invalid directive: line number out of range", num_start);
          // record the directive: subsequent lines are renumbered from <num> and
          // attributed to "<file>".  Anchor at the start of the next line.
          size_t anchor = (r < src_.size()) ? r + 1 : r;  // skip the trailing newline
          directives_.push_back({anchor, static_cast<int>(std::stoll(std::string(sig))),
                                 std::string(src_.substr(p + 1, q - (p + 1)))});
          pos_ = r;            // byte offsets are unaffected
          return raw_token();  // directive produces no token; continue
        }
      }
    }
    // not a well-formed directive: fall through to HASH / HASHOP
  }
  // "#" symbolchar_or_hash+ -> HASHOP ; else HASH
  if (is_symbolchar_or_hash(at(pos_ + 1))) {
    size_t q = pos_ + 1;
    while (q < src_.size() && is_symbolchar_or_hash(src_[q])) q++;
    Token t = Token::make(Kind::HASHOP, start, q);
    t.text = std::string(src_.substr(start, q - start));
    pos_ = q;
    return t;
  }
  pos_++;
  return Token::make(Kind::HASH, start, pos_);
}

Token Lexer::scan_symbol(size_t start) {
  char c = cur();

  // ".~" is a reserved sequence (MetaOCaml).
  if (c == '.' && at(pos_ + 1) == '~') throw LexError("Reserved sequence .~", start);

  // --- longest dedicated spelling ---
  size_t ded_len = 0;
  Kind ded_kind = Kind::KIND_COUNT;
  for (auto& [sp, k] : dedicated_symbols()) {
    if (sp.size() > ded_len && looking_at(sp)) {
      ded_len = sp.size();
      ded_kind = k;
    }
  }

  // --- longest open-ended operator run, classified by starter ---
  size_t open_len = 0;
  Kind open_kind = Kind::KIND_COUNT;
  auto run_symbolchar = [&](size_t from) {
    size_t q = from;
    while (q < src_.size() && is_symbolchar(src_[q])) q++;
    return q;
  };
  auto run_symbolchar_or_hash = [&](size_t from) {
    size_t q = from;
    while (q < src_.size() && is_symbolchar_or_hash(src_[q])) q++;
    return q;
  };

  switch (c) {
    case '!': {  // PREFIXOP, cont symbolchar_or_hash+ (len>=2)
      size_t q = run_symbolchar_or_hash(pos_ + 1);
      if (q - pos_ >= 2) { open_len = q - pos_; open_kind = Kind::PREFIXOP; }
      break;
    }
    case '=': case '<': case '>': case '|': case '&': case '$': {  // INFIXOP0
      size_t q = run_symbolchar(pos_ + 1);
      open_len = q - pos_; open_kind = Kind::INFIXOP0;
      break;
    }
    case '@': case '^': {  // INFIXOP1
      size_t q = run_symbolchar(pos_ + 1);
      open_len = q - pos_; open_kind = Kind::INFIXOP1;
      break;
    }
    case '+': case '-': {  // INFIXOP2
      size_t q = run_symbolchar(pos_ + 1);
      open_len = q - pos_; open_kind = Kind::INFIXOP2;
      break;
    }
    case '*': {  // "**" -> INFIXOP4 ; else INFIXOP3
      if (at(pos_ + 1) == '*') {
        size_t q = run_symbolchar(pos_ + 2);
        open_len = q - pos_; open_kind = Kind::INFIXOP4;
      } else {
        size_t q = run_symbolchar(pos_ + 1);
        open_len = q - pos_; open_kind = Kind::INFIXOP3;
      }
      break;
    }
    case '/': case '%': {  // INFIXOP3
      size_t q = run_symbolchar(pos_ + 1);
      open_len = q - pos_; open_kind = Kind::INFIXOP3;
      break;
    }
    case '.': {  // DOTOP: '.' dotsymbolchar symbolchar*  (len>=2, 2nd char dotsym)
      if (is_dotsymbolchar(at(pos_ + 1))) {
        size_t q = run_symbolchar(pos_ + 2);
        open_len = q - pos_; open_kind = Kind::DOTOP;
      }
      break;
    }
    default:
      break;  // ':' '~' '?' handled elsewhere or only dedicated
  }

  bool use_open = open_len > ded_len;  // ties -> dedicated (earlier rule)
  if (use_open) {
    Token t = Token::make(open_kind, start, pos_ + open_len);
    // DOTOP captures the operator *after* the leading '.'; all other open-ended
    // operators include their starter character.
    if (open_kind == Kind::DOTOP)
      t.text = std::string(src_.substr(start + 1, open_len - 1));
    else
      t.text = std::string(src_.substr(start, open_len));
    pos_ += open_len;
    return t;
  }
  if (ded_len > 0) {
    Token t = Token::make(ded_kind, start, pos_ + ded_len);
    // INFIXOP-valued dedicated tokens carry their spelling as text ("!=").
    if (ded_kind == Kind::INFIXOP0 || ded_kind == Kind::INFIXOP1 ||
        ded_kind == Kind::INFIXOP2 || ded_kind == Kind::INFIXOP3 ||
        ded_kind == Kind::INFIXOP4)
      t.text = std::string(src_.substr(start, ded_len));
    pos_ += ded_len;
    return t;
  }
  throw LexError("Illegal character", start);
}

}  // namespace cppcaml
