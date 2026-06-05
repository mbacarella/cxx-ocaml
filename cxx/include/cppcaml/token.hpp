// Token type for the C++ OCaml lexer.
//
// Faithful to parsing/parser.mly's %token declarations (124 kinds) and the
// payloads attached by parsing/lexer.mll. Tokens are modelled as a `Kind` enum
// plus a small payload struct — the same tagged-union shape OCaml itself uses
// for `Parser.token`. (The std::variant data model is reserved for the AST and
// type representations, where the constructor set is large and heterogeneous;
// for ~20 payload-carrying tokens an enum+fields is both faithful and compact.)
#pragma once
#include <cstddef>
#include <cstdint>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>

namespace cppcaml {

// The complete token set, kept in one X-macro list so the enum and the
// name table are generated from a single source of truth.
#define CPPCAML_TOKEN_LIST(X)                                                  \
  X(AMPERAMPER) X(AMPERSAND) X(AND) X(ANDOP) X(AS) X(ASSERT) X(BACKQUOTE)      \
  X(BANG) X(BAR) X(BARBAR) X(BARRBRACKET) X(BEGIN) X(CHAR) X(CLASS) X(COLON)   \
  X(COLONCOLON) X(COLONEQUAL) X(COLONGREATER) X(COMMA) X(COMMENT) X(CONSTRAINT)\
  X(DO) X(DOCSTRING) X(DONE) X(DOT) X(DOTDOT) X(DOTOP) X(DOWNTO) X(EFFECT)     \
  /* TEOF: 'EOF' is a <stdio.h> macro; printed name is still "EOF". */         \
  X(ELSE) X(END) X(TEOF) X(EOL) X(EQUAL) X(EXCEPTION) X(EXTERNAL) X(FALSE)     \
  X(FLOAT) X(FOR) X(FUN) X(FUNCTION) X(FUNCTOR) X(GREATER) X(GREATERRBRACE)    \
  X(GREATERRBRACKET) X(HASH) X(HASHOP) X(IF) X(IN) X(INCLUDE) X(INFIXOP0)      \
  X(INFIXOP1) X(INFIXOP2) X(INFIXOP3) X(INFIXOP4) X(INHERIT) X(INITIALIZER)    \
  X(INT) X(LABEL) X(LAZY) X(LBRACE) X(LBRACELESS) X(LBRACKET) X(LBRACKETAT)    \
  X(LBRACKETATAT) X(LBRACKETATATAT) X(LBRACKETBAR) X(LBRACKETGREATER)          \
  X(LBRACKETLESS) X(LBRACKETPERCENT) X(LBRACKETPERCENTPERCENT) X(LESS)         \
  X(LESSMINUS) X(LET) X(LETOP) X(LIDENT) X(LPAREN) X(MATCH)                    \
  X(METAOCAML_BRACKET_CLOSE) X(METAOCAML_BRACKET_OPEN) X(METAOCAML_ESCAPE)     \
  X(METHOD) X(MINUS) X(MINUSDOT) X(MINUSGREATER) X(MODULE) X(MUTABLE) X(NEW)   \
  X(NONREC) X(OBJECT) X(OF) X(OPEN) X(OPTLABEL) X(OR) X(PERCENT) X(PLUS)       \
  X(PLUSDOT) X(PLUSEQ) X(PREFIXOP) X(PRIVATE) X(QUESTION) X(QUOTE) X(RBRACE)   \
  X(RBRACKET) X(REC) X(RPAREN) X(SEMI) X(SEMISEMI) X(SIG) X(STAR) X(STRING)    \
  X(STRUCT) X(THEN) X(TILDE) X(TO) X(TRUE) X(TRY) X(TYPE) X(UIDENT)            \
  X(UNDERSCORE) X(VAL) X(VIRTUAL) X(WHEN) X(WHILE) X(WITH)                     \
  X(QUOTED_STRING_EXPR) X(QUOTED_STRING_ITEM)

enum class Kind : uint8_t {
#define X(name) name,
  CPPCAML_TOKEN_LIST(X)
#undef X
  KIND_COUNT
};

inline std::string_view kind_name(Kind k) {
  static constexpr std::string_view names[] = {
#define X(name) #name,
      CPPCAML_TOKEN_LIST(X)
#undef X
  };
  if (k == Kind::TEOF) return "EOF";  // canonical name despite enum rename
  return names[static_cast<size_t>(k)];
}

struct Token {
  Kind kind{};
  size_t start = 0;  // byte offset of first char of the lexeme
  size_t end = 0;    // byte offset one past the last char

  // Payloads (only the relevant one(s) are set for a given kind):
  std::string text;                  // ident/op/label name; literal text; STRING content
  std::optional<char> modifier;      // INT/FLOAT trailing literal modifier (e.g. 'L','g')
  std::optional<std::string> delim;  // STRING/quoted-string delimiter (nullopt => "..." form)
  std::string ext_id;                // QUOTED_STRING_EXPR/ITEM extension identifier
  size_t content_start = 0;          // QUOTED_STRING_*: byte offset of the content (after `|`)
  int char_code = -1;                // CHAR: byte value 0..255

  static Token make(Kind k, size_t s, size_t e) { return Token{k, s, e}; }
};

// Canonical one-line rendering, identical on the OCaml oracle side, so token
// streams can be diffed byte-for-byte. Format:
//   <KIND>[ <payload fields...>]\t<start>\t<end>
// Payload-free kinds print just the name. See oracle/dump_tokens.ml for the
// matching OCaml printer.
void print_canonical(const Token& t, std::ostream& os);

}  // namespace cppcaml
