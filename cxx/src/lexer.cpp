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

  if (is_lower(c)) return scan_ident_lower(start);
  if (is_upper(c)) return scan_ident_upper(start);
  if (static_cast<unsigned char>(c) >= 0xC0) {
    // TODO: full UTF-8 extended identifiers w/ normalization + validation.
    // Best-effort: consume a UTF-8 ident run and emit LIDENT.
    pos_++;
    while (!eof() &&
           (is_identchar(cur()) || static_cast<unsigned char>(cur()) >= 0x80))
      pos_++;
    Token t = Token::make(Kind::LIDENT, start, pos_);
    t.text = std::string(src_.substr(start, pos_ - start));
    return t;
  }
  if (is_digit(c)) return scan_number(start);
  if (c == '"') return scan_string(start);
  if (c == '\'') return scan_char_or_quote(start);
  if (c == '(') {
    if (at(pos_ + 1) == '*') {
      scan_comment();
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

std::vector<Token> Lexer::tokenize() {
  std::vector<Token> out;
  for (;;) {
    Token t = next();
    out.push_back(t);
    if (t.kind == Kind::TEOF) break;
  }
  return out;
}

Token Lexer::scan_ident_lower(size_t start) {
  pos_++;
  while (!eof() && is_identchar(cur())) pos_++;
  std::string name(src_.substr(start, pos_ - start));

  if (name == "_") return Token::make(Kind::UNDERSCORE, start, pos_);

  // let-op / and-op: "let"/"and" immediately followed by kwdopchar.
  if ((name == "let" || name == "and") && is_kwdopchar(cur())) {
    size_t op_start = start;
    pos_++;  // kwdopchar
    while (!eof() && is_dotsymbolchar(cur())) pos_++;
    Token t = Token::make(name == "let" ? Kind::LETOP : Kind::ANDOP, op_start, pos_);
    t.text = std::string(src_.substr(op_start, pos_ - op_start));
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

Token Lexer::scan_ident_upper(size_t start) {
  pos_++;
  while (!eof() && is_identchar(cur())) pos_++;
  Token t = Token::make(Kind::UIDENT, start, pos_);
  t.text = std::string(src_.substr(start, pos_ - start));
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
        store(static_cast<char>(100 * (e - '0') + 10 * (at(pos_ + 2) - '0') +
                                (at(pos_ + 3) - '0')));
        pos_ += 4;
        continue;
      }
      if (e == 'o') {
        store(static_cast<char>(64 * (at(pos_ + 2) - '0') + 8 * (at(pos_ + 3) - '0') +
                                (at(pos_ + 4) - '0')));
        pos_ += 5;
        continue;
      }
      if (e == 'x' && is_hex(at(pos_ + 2)) && is_hex(at(pos_ + 3))) {
        store(static_cast<char>(16 * digit_value(at(pos_ + 2)) + digit_value(at(pos_ + 3))));
        pos_ += 5;
        continue;
      }
      if (e == 'u' && at(pos_ + 2) == '{') {
        // \u{HHH} -> UTF-8 encode the scalar value.
        size_t q = pos_ + 3;
        int cp = 0;
        while (is_hex(at(q))) { cp = cp * 16 + digit_value(at(q)); q++; }
        // (assume well-formed '}'; TODO: validate scalar value range)
        if (at(q) == '}') q++;
        // UTF-8 encode
        if (cp < 0x80) {
          store(static_cast<char>(cp));
        } else if (cp < 0x800) {
          store(static_cast<char>(0xC0 | (cp >> 6)));
          store(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
          store(static_cast<char>(0xE0 | (cp >> 12)));
          store(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
          store(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
          store(static_cast<char>(0xF0 | (cp >> 18)));
          store(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
          store(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
          store(static_cast<char>(0x80 | (cp & 0x3F)));
        }
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
  // "{" delim_ext "|"  -> quoted string ;  "{<" -> LBRACELESS ; else LBRACE
  // (TODO: {% extattrident | and {%% ... | quoted-string extensions)
  if (looking_at("{%")) {
    // best-effort: not yet supported; fall through to LBRACE-ish handling later.
    // For now emit LBRACE and let the '%' be lexed next (will diverge; tracked).
    // TODO: QUOTED_STRING_EXPR / QUOTED_STRING_ITEM.
  }
  // try delim_ext then '|'
  size_t p = pos_ + 1;
  while (p < src_.size() &&
         ((src_[p] >= 'a' && src_[p] <= 'z') || (src_[p] >= 'A' && src_[p] <= 'Z') ||
          static_cast<unsigned char>(src_[p]) >= 0xC0))
    p++;
  if (p < src_.size() && src_[p] == '|') {
    std::string delim(src_.substr(pos_ + 1, p - (pos_ + 1)));
    pos_ = p + 1;  // past '|'
    return scan_quoted_string(start, delim);
  }
  if (at(pos_ + 1) == '<') { pos_ += 2; return Token::make(Kind::LBRACELESS, start, pos_); }
  pos_++;
  return Token::make(Kind::LBRACE, start, pos_);
}

Token Lexer::scan_hash(size_t start) {
  // TODO: line directives (# <num> "<file>") at beginning of line.
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
