// Hand-written lexer for OCaml source, ported from parsing/lexer.mll.
//
// Produces the *filtered* token stream that the parser consumes: COMMENT, EOL
// and DOCSTRING are recognised but dropped by next(), exactly as the OCaml
// `Lexer.token` wrapper does. raw_token() exposes the unfiltered stream.
#pragma once
#include <optional>
#include <cstddef>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "cppcaml/token.hpp"

namespace cppcaml {

// A `(** … *)` doc comment: body (delimiters stripped) and the comment's span.
// [id] is the registration index (Docstrings.register order) into DocAttach::all.
struct Docstring { std::string body; size_t start = 0; size_t end = 0; size_t id = 0; };

// Docstring attachment tables, keyed by byte offset (cnum), built by the lexer's
// token/EOL/docstring state machine (ports parsing/docstrings.ml + lexer.mll).
//   pre[start]   -> ocaml.doc before the item starting there  (docs_pre)
//   post[end]    -> ocaml.doc after the item ending there     (docs_post)
//   floating[s]  -> ocaml.text items emitted before the item starting at s
//   pre_extra[s] / post_extra[e] -> ocaml.text threaded at structure boundaries
struct DocAttach {
  std::unordered_map<size_t, std::vector<Docstring>> pre, post, floating, pre_extra, post_extra;
  std::vector<Docstring> all;  // Docstrings.docstrings: every docstring, in source order
};

// Lexing error, mirrors lexer.mll's `Error of error * Location.t`: the
// error (kind, its string payload and optional explanation) and the byte
// span of its location.
struct LexError : std::runtime_error {
  enum class Kind {
    Illegal_character, Illegal_escape, Reserved_sequence, Unterminated_comment, Unterminated_string,
    Unterminated_string_in_comment,
    Empty_character_literal, Invalid_literal, Invalid_directive, Invalid_encoding, Invalid_char_in_ident,
    Non_lowercase_delimiter, Capitalized_raw_identifier, Unknown_keyword, Other
  };
  Kind kind = Kind::Other;
  size_t pos;                        // the location: [pos, end)
  size_t end;
  std::string arg;                   // the error's string (or char / code point) payload
  std::optional<std::string> expl;   // Illegal_escape / Reserved_sequence / Invalid_directive
  size_t pos2 = 0;                   // Unterminated_string_in_comment: the literal's start
  LexError(std::string msg, size_t p) : std::runtime_error(std::move(msg)), pos(p), end(p) {}
  LexError(Kind k, size_t p, size_t e, std::string a = {}, std::optional<std::string> x = std::nullopt)
      : std::runtime_error("Lexer.Error"), kind(k), pos(p), end(e), arg(std::move(a)), expl(std::move(x)) {}
};

// Warnings the lexer reports (lexer.mll's Location.prerr_warning): recorded
// in source order by the last tokenize(), for the driver to print with the
// parse (Comment_start = 1, Comment_not_end = 2, Illegal_backslash = 14).
struct LexWarning {
  int number;
  size_t start, end;
  bool flag = true;  // Unexpected_docstring's payload (50: unattached, not ambiguous)
};
std::vector<LexWarning>& lex_warnings();

class Lexer {
 public:
  // `src` must outlive the lexer (we hold a view into it).
  explicit Lexer(std::string_view src) : src_(src) {}

  // Next parser-visible token (COMMENT/EOL/DOCSTRING skipped). Returns EOF
  // repeatedly at end of input.
  Token next();

  // Unfiltered single step; may return COMMENT/EOL/DOCSTRING.
  Token raw_token();

  // Convenience: lex the whole buffer into the filtered token vector,
  // terminated by an EOF token. Also populates the docstring attachment tables.
  // The tokens, up to the first lexer error: that error ends the stream with
  // a TEOF marked lex_error, and is kept in pending_error() -- ocamlc lexes on
  // demand, so the error only arises when the parser reaches it.
  std::vector<Token> tokenize();
  const std::optional<LexError>& pending_error() const { return pending_error_; }

  const DocAttach& doc_attach() const { return docs_; }

  // `# N "file"` line directives, in source order (anchor = next line's start).
  struct LineDirective { size_t anchor_cnum; int line; std::string file; };
  const std::vector<LineDirective>& directives() const { return directives_; }
  // Lexer.comments (): every comment (a docstring as "*" ^ its body), with
  // its span, in source order (tokenize fills it)
  struct Comment { std::string text; size_t start = 0; size_t end = 0; };
  const std::vector<Comment>& comments() const { return comments_; }

 private:
  std::string_view src_;
  size_t pos_ = 0;
  DocAttach docs_;
  std::vector<LineDirective> directives_;
  std::vector<Comment> comments_;

  bool is_doc_comment(size_t s, size_t e) const;
  std::string doc_body(size_t s, size_t e) const;

  // --- cursor helpers ---
  bool eof() const { return pos_ >= src_.size(); }
  char cur() const { return pos_ < src_.size() ? src_[pos_] : '\0'; }
  char at(size_t i) const { return i < src_.size() ? src_[i] : '\0'; }
  bool looking_at(std::string_view s) const { return src_.substr(pos_).starts_with(s); }

  // --- sub-lexers (mirror the named rules in lexer.mll) ---
  Token scan_ident(size_t start);  // ASCII + Latin-9 extended identifiers
  Token scan_number(size_t start);
  Token scan_char_or_quote(size_t start);
  Token scan_string(size_t start);     // after opening '"'
  Token scan_quoted_string(size_t start, const std::string& delim);  // after {delim|
  void scan_comment();                 // after "(*", consumes through matching "*)"
  Token scan_label_or_tilde(size_t start);
  Token scan_optlabel_or_question(size_t start);
  Token scan_brace(size_t start);      // '{', "{<", "{|...", "{%..."
  Token scan_hash(size_t start);       // '#', directive, HASHOP
  Token scan_symbol(size_t start);     // operators + symbolic punctuation

  // string-literal accumulation buffer (lexer.mll's string_buffer)
  std::string strbuf_;
  void store(char c) { strbuf_.push_back(c); }
  void store(std::string_view s) { strbuf_.append(s); }
  std::optional<LexError> pending_error_;
};

// True if `s` is an OCaml keyword (matches Lexer.is_keyword); used by the printer
// to escape type variables named like keywords (`'\#let`).
bool is_ocaml_keyword(std::string_view s);
// Lexer.init ~keyword_edition (populate_keywords): (None, []) is the
// default edition, every keyword
void set_keyword_edition(std::optional<std::pair<int, int>> version, const std::vector<std::string>& keywords);

}  // namespace cppcaml
