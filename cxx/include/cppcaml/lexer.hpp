// Hand-written lexer for OCaml source, ported from parsing/lexer.mll.
//
// Produces the *filtered* token stream that the parser consumes: COMMENT, EOL
// and DOCSTRING are recognised but dropped by next(), exactly as the OCaml
// `Lexer.token` wrapper does. raw_token() exposes the unfiltered stream.
#pragma once
#include <cstddef>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#include "cppcaml/token.hpp"

namespace cppcaml {

// A `(** … *)` doc comment: body (delimiters stripped) and the comment's span.
struct Docstring { std::string body; size_t start = 0; size_t end = 0; };

// Docstring attachment tables, keyed by byte offset (cnum), built by the lexer's
// token/EOL/docstring state machine (ports parsing/docstrings.ml + lexer.mll).
//   pre[start]   -> ocaml.doc before the item starting there  (docs_pre)
//   post[end]    -> ocaml.doc after the item ending there     (docs_post)
//   floating[s]  -> ocaml.text items emitted before the item starting at s
//   pre_extra[s] / post_extra[e] -> ocaml.text threaded at structure boundaries
struct DocAttach {
  std::unordered_map<size_t, std::vector<Docstring>> pre, post, floating, pre_extra, post_extra;
};

// Lexing error, mirrors lexer.mll's `Error of error * Location.t`. For now we
// carry a message and the byte offset; structured error variants come later.
struct LexError : std::runtime_error {
  size_t pos;
  LexError(std::string msg, size_t p)
      : std::runtime_error(std::move(msg)), pos(p) {}
};

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
  std::vector<Token> tokenize();

  const DocAttach& doc_attach() const { return docs_; }

  // `# N "file"` line directives, in source order (anchor = next line's start).
  struct LineDirective { size_t anchor_cnum; int line; std::string file; };
  const std::vector<LineDirective>& directives() const { return directives_; }

 private:
  std::string_view src_;
  size_t pos_ = 0;
  DocAttach docs_;
  std::vector<LineDirective> directives_;

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
};

}  // namespace cppcaml
