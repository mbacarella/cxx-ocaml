// Recursive-descent + Pratt parser; see parser.hpp. Mirrors the menhir grammar's
// node shapes, locations (incl. ghost locs for desugared function bindings), and
// operator precedence, validated against `ocamlc -dparsetree`.
#include "cppcaml/parser.hpp"

#include <optional>
#include <utility>
#include <vector>

#include "cppcaml/lexer.hpp"

namespace cppcaml {
using namespace ast;

namespace {

template <class T>
Box<T> box(T v) { return std::make_unique<T>(std::move(v)); }
ExprBox E(Expression e) { return std::make_unique<Expression>(std::move(e)); }

struct OpInfo { int prec; bool right; std::string name; };

// OCaml infix operator precedence/associativity (higher = binds tighter).
std::optional<OpInfo> infix_op(const Token& t) {
  switch (t.kind) {
    case Kind::COLONEQUAL: return OpInfo{1, true, ":="};
    case Kind::LESSMINUS:  return OpInfo{1, true, "<-"};
    case Kind::BARBAR:     return OpInfo{2, true, "||"};
    case Kind::AMPERSAND:  return OpInfo{3, true, "&"};
    case Kind::AMPERAMPER: return OpInfo{3, true, "&&"};
    case Kind::EQUAL:      return OpInfo{4, false, "="};
    case Kind::LESS:       return OpInfo{4, false, "<"};
    case Kind::GREATER:    return OpInfo{4, false, ">"};
    case Kind::INFIXOP0:   return OpInfo{4, false, t.text};
    case Kind::INFIXOP1:   return OpInfo{5, true, t.text};
    case Kind::PLUS:       return OpInfo{7, false, "+"};
    case Kind::PLUSDOT:    return OpInfo{7, false, "+."};
    case Kind::MINUS:      return OpInfo{7, false, "-"};
    case Kind::MINUSDOT:   return OpInfo{7, false, "-."};
    case Kind::INFIXOP2:   return OpInfo{7, false, t.text};
    case Kind::STAR:       return OpInfo{8, false, "*"};
    case Kind::PERCENT:    return OpInfo{8, false, "%"};
    case Kind::INFIXOP3:   return OpInfo{8, false, t.text};
    case Kind::INFIXOP4:   return OpInfo{9, true, t.text};
    default: return std::nullopt;
  }
}

bool is_atom_start(Kind k) {
  switch (k) {
    case Kind::INT: case Kind::FLOAT: case Kind::CHAR: case Kind::STRING:
    case Kind::LIDENT: case Kind::UIDENT: case Kind::LPAREN:
      return true;
    default:
      return false;
  }
}

class Parser {
 public:
  explicit Parser(std::string_view src) : src_(src) {
    tokens_ = Lexer(src).tokenize();
    line_starts_.push_back(0);
    for (size_t i = 0; i < src.size(); ++i)
      if (src[i] == '\n') line_starts_.push_back(static_cast<int>(i + 1));
  }

  Structure parse_structure() {
    Structure items;
    while (cur().kind != Kind::TEOF) {
      if (cur().kind == Kind::SEMISEMI) { advance(); continue; }
      items.push_back(parse_structure_item());
    }
    return items;
  }

 private:
  std::string_view src_;
  std::vector<Token> tokens_;
  std::vector<int> line_starts_;
  size_t idx_ = 0;

  const Token& cur() const { return tokens_[idx_]; }
  void advance() { if (idx_ + 1 < tokens_.size()) idx_++; }
  void expect(Kind k, const char* what) {
    if (cur().kind != k) throw ParseError(std::string("expected ") + what, cur().start);
    advance();
  }

  Position position(size_t cnum) const {
    int lo = 0, hi = static_cast<int>(line_starts_.size()) - 1, ans = 0;
    while (lo <= hi) {
      int mid = (lo + hi) / 2;
      if (line_starts_[mid] <= static_cast<int>(cnum)) { ans = mid; lo = mid + 1; }
      else hi = mid - 1;
    }
    return Position{ans + 1, line_starts_[ans], static_cast<int>(cnum)};
  }
  Location tokloc(const Token& t) const {
    return Location{position(t.start), position(t.end), false};
  }
  Location span(Position a, Position b, bool ghost = false) const {
    return Location{a, b, ghost};
  }

  Constant const_of(const Token& t) const {
    Location l = tokloc(t);
    switch (t.kind) {
      case Kind::INT:   return Constant{Pconst_integer{t.text, t.modifier}, l};
      case Kind::FLOAT: return Constant{Pconst_float{t.text, t.modifier}, l};
      case Kind::CHAR:  return Constant{Pconst_char{t.char_code}, l};
      default:          return Constant{Pconst_string{t.text, l, t.delim}, l};  // STRING
    }
  }

  // ---- expressions ----
  ExprBox parse_atom() {
    Token t = cur();
    switch (t.kind) {
      case Kind::INT: case Kind::FLOAT: case Kind::CHAR: case Kind::STRING: {
        advance();
        Location l = tokloc(t);
        return E({Pexp_constant{const_of(t)}, l});
      }
      case Kind::LIDENT: {
        advance();
        Location l = tokloc(t);
        return E({Pexp_ident{LongidentLoc{{Lident{t.text}}, l}}, l});
      }
      case Kind::UIDENT: {
        // qualified value path  Uid.(Uid.)*lid  -> Pexp_ident (Ldot ...)
        Token first = t;
        advance();
        Longident lid{Lident{first.text}};
        Token last = first;
        while (cur().kind == Kind::DOT) {
          advance();
          Token nm = cur();
          if (nm.kind != Kind::LIDENT && nm.kind != Kind::UIDENT)
            throw ParseError("expected identifier after '.'", nm.start);
          advance();
          lid = Longident{Ldot{box(std::move(lid)), nm.text}};
          last = nm;
        }
        if (std::holds_alternative<Lident>(lid.v))
          throw ParseError("bare constructor not supported yet", first.start);
        Location l = span(position(first.start), position(last.end));
        return E({Pexp_ident{LongidentLoc{std::move(lid), l}}, l});
      }
      case Kind::LPAREN: {
        advance();
        ExprBox inner = parse_expr();
        Token close = cur();
        expect(Kind::RPAREN, ")");
        inner->loc = span(position(t.start), position(close.end));  // reloc to parens
        return inner;
      }
      default:
        throw ParseError("expected an expression", t.start);
    }
  }

  ExprBox parse_app() {
    ExprBox fn = parse_atom();
    std::vector<std::pair<ArgLabel, ExprBox>> args;
    while (is_atom_start(cur().kind)) args.emplace_back(Nolabel{}, parse_atom());
    if (args.empty()) return fn;
    Location l = span(fn->loc.start, args.back().second->loc.end);
    return E({Pexp_apply{std::move(fn), std::move(args)}, l});
  }

  ExprBox parse_binop(int min_prec) {
    ExprBox left = parse_app();
    for (;;) {
      auto op = infix_op(cur());
      if (!op || op->prec < min_prec) break;
      Token optok = cur();
      advance();
      ExprBox right = parse_binop(op->right ? op->prec : op->prec + 1);
      Location opl = tokloc(optok);
      ExprBox fn = E({Pexp_ident{LongidentLoc{{Lident{op->name}}, opl}}, opl});
      Location l = span(left->loc.start, right->loc.end);
      std::vector<std::pair<ArgLabel, ExprBox>> args;
      args.emplace_back(Nolabel{}, std::move(left));
      args.emplace_back(Nolabel{}, std::move(right));
      left = E({Pexp_apply{std::move(fn), std::move(args)}, l});
    }
    return left;
  }

  ExprBox parse_tuple() {
    ExprBox first = parse_binop(0);
    if (cur().kind != Kind::COMMA) return first;
    std::vector<ExprBox> elems;
    Position s = first->loc.start;
    elems.push_back(std::move(first));
    while (cur().kind == Kind::COMMA) { advance(); elems.push_back(parse_binop(0)); }
    Location l = span(s, elems.back()->loc.end);
    return E({Pexp_tuple{std::move(elems)}, l});
  }

  ExprBox parse_expr() {
    Token t = cur();
    switch (t.kind) {
      case Kind::LET: {
        auto [rf, binds] = parse_value_bindings();
        expect(Kind::IN, "in");
        ExprBox body = parse_expr();
        Location l = span(position(t.start), body->loc.end);
        return E({Pexp_let{rf, std::move(binds), std::move(body)}, l});
      }
      case Kind::IF: {
        advance();
        ExprBox c = parse_expr();
        expect(Kind::THEN, "then");
        ExprBox th = parse_expr();
        std::optional<ExprBox> el;
        if (cur().kind == Kind::ELSE) { advance(); el = parse_expr(); }
        Position end = el ? (*el)->loc.end : th->loc.end;
        Location l = span(position(t.start), end);
        return E({Pexp_ifthenelse{std::move(c), std::move(th), std::move(el)}, l});
      }
      default:
        return parse_tuple();
    }
  }

  // ---- bindings ----
  FunctionParam parse_param() {
    Token t = cur();
    Location l = tokloc(t);
    Pattern p;
    if (t.kind == Kind::LIDENT) { advance(); p = Pattern{Ppat_var{StringLoc{t.text, l}}, l}; }
    else if (t.kind == Kind::UNDERSCORE) { advance(); p = Pattern{Ppat_any{}, l}; }
    else throw ParseError("unsupported parameter pattern", t.start);
    return FunctionParam{Pparam_val{l, Nolabel{}, std::move(p)}};
  }

  ValueBinding parse_value_binding() {
    Token nt = cur();
    if (nt.kind != Kind::LIDENT)
      throw ParseError("unsupported let-binding pattern", nt.start);
    advance();
    Location nl = tokloc(nt);
    Pattern namepat{Ppat_var{StringLoc{nt.text, nl}}, nl};

    std::vector<FunctionParam> params;
    while (cur().kind != Kind::EQUAL) params.push_back(parse_param());
    expect(Kind::EQUAL, "=");
    ExprBox body = parse_expr();

    if (params.empty()) return ValueBinding{std::move(namepat), std::move(body)};
    // desugar `let f p.. = e` to a ghost Pexp_function spanning p[0]..e
    Position fstart = std::get<Pparam_val>(params.front().desc).loc.start;
    Location floc = span(fstart, body->loc.end, /*ghost=*/true);
    auto fb = box(FunctionBody{Pfunction_body{std::move(body)}});
    ExprBox fn = E({Pexp_function{std::move(params), std::move(fb)}, floc});
    return ValueBinding{std::move(namepat), std::move(fn)};
  }

  std::pair<RecFlag, std::vector<ValueBinding>> parse_value_bindings() {
    expect(Kind::LET, "let");
    RecFlag rf = RecFlag::Nonrecursive;
    if (cur().kind == Kind::REC) { advance(); rf = RecFlag::Recursive; }
    std::vector<ValueBinding> binds;
    binds.push_back(parse_value_binding());
    while (cur().kind == Kind::AND) { advance(); binds.push_back(parse_value_binding()); }
    return {rf, std::move(binds)};
  }

  // ---- structure ----
  StructureItem parse_structure_item() {
    Token t = cur();
    if (t.kind == Kind::LET) {
      size_t save = idx_;
      auto [rf, binds] = parse_value_bindings();
      if (cur().kind == Kind::IN) {  // it's actually a let-expression statement
        idx_ = save;
        ExprBox e = parse_expr();
        Location l = e->loc;
        return StructureItem{Pstr_eval{std::move(e)}, l};
      }
      Location l = span(position(t.start), position(tokens_[idx_ - 1].end));
      return StructureItem{Pstr_value{rf, std::move(binds)}, l};
    }
    ExprBox e = parse_expr();
    Location l = e->loc;
    return StructureItem{Pstr_eval{std::move(e)}, l};
  }
};

}  // namespace

Structure parse_structure(std::string_view src) { return Parser(src).parse_structure(); }

}  // namespace cppcaml
