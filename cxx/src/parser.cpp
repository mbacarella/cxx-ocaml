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
    case Kind::TRUE: case Kind::FALSE: case Kind::LBRACKET: case Kind::LBRACE:
    case Kind::BANG: case Kind::PREFIXOP: case Kind::LBRACKETBAR:
    case Kind::BEGIN:
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
  const Token& peek(size_t n) const {
    size_t k = idx_ + n;
    return tokens_[k < tokens_.size() ? k : tokens_.size() - 1];
  }
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
  LongidentLoc lid0(std::string name, Location l) { return LongidentLoc{{Lident{std::move(name)}}, l}; }
  ExprBox mk_construct(LongidentLoc cl, std::optional<ExprBox> arg, Location l) {
    return E({Pexp_construct{.id = cl, .arg = std::move(arg)}, l});
  }
  Location gloc(Position a, Position b) { return Location{a, b, true}; }

  // [e1; ...; en] desugars right-assoc to (::) chains (mktailexp). The cons/tuple
  // nodes are ghost; only the outermost expression carries the real bracket span.
  ExprBox build_expr_list(std::vector<ExprBox>& elems, Position lb, Position rbS, Position rbE) {
    ExprBox acc = mk_construct(lid0("[]", gloc(rbS, rbE)), std::nullopt, gloc(rbS, rbE));
    for (int i = static_cast<int>(elems.size()) - 1; i >= 0; --i) {
      Position es = elems[i]->loc.start;
      Location gl = gloc(es, rbE);
      std::vector<ExprBox> tup;
      tup.push_back(std::move(elems[i]));
      tup.push_back(std::move(acc));
      acc = mk_construct(lid0("::", gl), E({Pexp_tuple{std::move(tup)}, gl}), gl);
    }
    acc->loc = Location{lb, rbE, false};  // outermost: real bracket span
    return acc;
  }
  std::string lid_last_name(const Longident& l) {
    if (auto* p = std::get_if<Lident>(&l.v)) return p->name;
    if (auto* p = std::get_if<Ldot>(&l.v)) return p->name;
    return "";
  }

  // A dotted path; reports whether the final segment is uppercase (constructor)
  // vs lowercase (value/field).
  struct PathResult { LongidentLoc lid; bool final_upper; };
  PathResult parse_dotted_path() {
    Token first = cur();
    advance();
    Longident lid{Lident{first.text}};
    Token last = first;
    bool upper = first.kind == Kind::UIDENT;
    while (cur().kind == Kind::DOT &&
           (peek(1).kind == Kind::LIDENT || peek(1).kind == Kind::UIDENT)) {
      advance();
      Token nm = cur();
      advance();
      lid = Longident{Ldot{std::make_shared<Longident>(std::move(lid)), nm.text}};
      last = nm;
      upper = nm.kind == Kind::UIDENT;
    }
    return {LongidentLoc{std::move(lid), span(position(first.start), position(last.end))}, upper};
  }

  // postfix record-field access  e.lbl (.lbl)*
  ExprBox qualified_ident(const char* mod, const char* fn, Location l) {
    Longident lid{Ldot{std::make_shared<Longident>(Longident{Lident{mod}}), fn}};
    return E({Pexp_ident{.id = LongidentLoc{std::move(lid), l}}, l});
  }
  ExprBox indexed_get(ExprBox e, ExprBox idx, Position end, const char* mod) {
    Position s = e->loc.start;
    ExprBox fn = qualified_ident(mod, "get", Location{s, end, true});  // ghost ident
    std::vector<std::pair<ArgLabel, ExprBox>> args;
    args.emplace_back(Nolabel{}, std::move(e));
    args.emplace_back(Nolabel{}, std::move(idx));
    return E({Pexp_apply{std::move(fn), std::move(args)}, Location{s, end, false}});
  }
  ExprBox postfix_field(ExprBox e) {
    for (;;) {
      if (cur().kind == Kind::DOT && peek(1).kind == Kind::LIDENT) {
        advance();
        Token f = cur();
        advance();
        Location l = span(e->loc.start, position(f.end));
        e = E({Pexp_field{std::move(e), lid0(f.text, tokloc(f))}, l});
      } else if (cur().kind == Kind::DOT && peek(1).kind == Kind::LPAREN) {
        advance(); advance();  // . (
        ExprBox idx = parse_expr();
        Token c = cur(); expect(Kind::RPAREN, ")");
        e = indexed_get(std::move(e), std::move(idx), position(c.end), "Array");
      } else if (cur().kind == Kind::DOT && peek(1).kind == Kind::LBRACKET) {
        advance(); advance();  // . [
        ExprBox idx = parse_expr();
        Token c = cur(); expect(Kind::RBRACKET, "]");
        e = indexed_get(std::move(e), std::move(idx), position(c.end), "String");
      } else {
        break;
      }
    }
    return e;
  }

  ExprBox parse_atom() {
    Token t = cur();
    switch (t.kind) {
      case Kind::INT: case Kind::FLOAT: case Kind::CHAR: case Kind::STRING:
        advance();
        return E({Pexp_constant{const_of(t)}, tokloc(t)});
      case Kind::LIDENT: {
        advance();
        Location l = tokloc(t);
        return E({Pexp_ident{LongidentLoc{{Lident{t.text}}, l}}, l});
      }
      case Kind::UIDENT: {
        PathResult pr = parse_dotted_path();
        if (pr.final_upper) return mk_construct(pr.lid, std::nullopt, pr.lid.loc);
        Location l = pr.lid.loc;
        return E({Pexp_ident{.id = std::move(pr.lid)}, l});
      }
      case Kind::TRUE: advance(); return mk_construct(lid0("true", tokloc(t)), std::nullopt, tokloc(t));
      case Kind::FALSE: advance(); return mk_construct(lid0("false", tokloc(t)), std::nullopt, tokloc(t));
      case Kind::LPAREN: {
        advance();
        if (cur().kind == Kind::RPAREN) {
          Token c = cur(); advance();
          Location l = span(position(t.start), position(c.end));
          return mk_construct(lid0("()", l), std::nullopt, l);
        }
        ExprBox inner = parse_expr();
        if (cur().kind == Kind::COLON) {
          advance();
          CoreTypeBox ty = parse_core_type();
          Token c = cur(); expect(Kind::RPAREN, ")");
          return E({Pexp_constraint{std::move(inner), std::move(ty)},
                    span(position(t.start), position(c.end))});
        }
        Token c = cur(); expect(Kind::RPAREN, ")");
        inner->loc = span(position(t.start), position(c.end));  // reloc to parens
        return inner;
      }
      case Kind::LBRACKET: {
        advance();
        if (cur().kind == Kind::RBRACKET) {
          Token c = cur(); advance();
          Location l = span(position(t.start), position(c.end));
          return mk_construct(lid0("[]", l), std::nullopt, l);
        }
        std::vector<ExprBox> elems;
        elems.push_back(parse_expr_no_seq());
        while (cur().kind == Kind::SEMI) {
          advance();
          if (cur().kind == Kind::RBRACKET) break;  // trailing ';'
          elems.push_back(parse_expr_no_seq());
        }
        Token c = cur(); expect(Kind::RBRACKET, "]");
        return build_expr_list(elems, position(t.start), position(c.start), position(c.end));
      }
      case Kind::LBRACE: {
        advance();
        std::optional<ExprBox> base;
        if (is_atom_start(cur().kind) && cur().kind != Kind::RBRACE) {
          size_t save = idx_;
          ExprBox e = parse_atom_postfix();
          if (cur().kind == Kind::WITH) { advance(); base = std::move(e); }
          else idx_ = save;  // it was the first field label, not a base
        }
        std::vector<std::pair<LongidentLoc, ExprBox>> fields;
        while (cur().kind != Kind::RBRACE) {
          LongidentLoc lbl = parse_longident_path();
          ExprBox val;
          if (cur().kind == Kind::EQUAL) { advance(); val = parse_expr_no_seq(); }
          else val = E({Pexp_ident{.id = lbl}, lbl.loc});  // punning { x }
          fields.emplace_back(lbl, std::move(val));
          if (cur().kind == Kind::SEMI) advance(); else break;
        }
        Token c = cur(); expect(Kind::RBRACE, "}");
        return E({Pexp_record{std::move(fields), std::move(base)},
                  span(position(t.start), position(c.end))});
      }
      case Kind::BEGIN: {
        advance();
        ExprBox inner = parse_expr();
        Token c = cur(); expect(Kind::END, "end");
        inner->loc = span(position(t.start), position(c.end));
        return inner;
      }
      case Kind::BANG: case Kind::PREFIXOP: {
        advance();
        ExprBox arg = parse_atom_postfix();
        Location opl = tokloc(t);
        std::string nm = t.kind == Kind::BANG ? "!" : t.text;
        ExprBox fn = E({Pexp_ident{.id = lid0(nm, opl)}, opl});
        Position ae = arg->loc.end;
        std::vector<std::pair<ArgLabel, ExprBox>> args;
        args.emplace_back(Nolabel{}, std::move(arg));
        return E({Pexp_apply{std::move(fn), std::move(args)}, span(position(t.start), ae)});
      }
      case Kind::LBRACKETBAR: {
        advance();
        std::vector<ExprBox> elems;
        if (cur().kind != Kind::BARRBRACKET) {
          elems.push_back(parse_expr_no_seq());
          while (cur().kind == Kind::SEMI) {
            advance();
            if (cur().kind == Kind::BARRBRACKET) break;
            elems.push_back(parse_expr_no_seq());
          }
        }
        Token c = cur(); expect(Kind::BARRBRACKET, "|]");
        return E({Pexp_array{std::move(elems)}, span(position(t.start), position(c.end))});
      }
      default:
        throw ParseError("expected an expression", t.start);
    }
  }

  ExprBox parse_atom_postfix() { return postfix_field(parse_atom()); }

  ExprBox ident_expr(const std::string& name, Location l) {
    return E({Pexp_ident{.id = lid0(name, l)}, l});
  }
  ExprBox collect_app(ExprBox head) {
    std::vector<std::pair<ArgLabel, ExprBox>> args;
    for (;;) {
      Kind k = cur().kind;
      if (k == Kind::LABEL) {  // ~lbl:e
        Token lt = cur(); advance();
        args.emplace_back(Labelled{lt.text}, parse_atom_postfix());
      } else if (k == Kind::OPTLABEL) {  // ?lbl:e
        Token lt = cur(); advance();
        args.emplace_back(Optional{lt.text}, parse_atom_postfix());
      } else if (k == Kind::TILDE && peek(1).kind == Kind::LIDENT) {  // ~x punning
        advance(); Token id = cur(); advance();
        args.emplace_back(Labelled{id.text}, ident_expr(id.text, tokloc(id)));
      } else if (k == Kind::QUESTION && peek(1).kind == Kind::LIDENT) {  // ?x punning
        advance(); Token id = cur(); advance();
        args.emplace_back(Optional{id.text}, ident_expr(id.text, tokloc(id)));
      } else if (is_atom_start(k)) {
        args.emplace_back(Nolabel{}, parse_atom_postfix());
      } else {
        break;
      }
    }
    if (args.empty()) return head;
    Location l = span(head->loc.start, args.back().second->loc.end);
    return E({Pexp_apply{std::move(head), std::move(args)}, l});
  }

  ExprBox parse_app() {
    if (cur().kind == Kind::ASSERT) {
      Token t = cur(); advance();
      ExprBox arg = parse_atom_postfix();
      Position ae = arg->loc.end;
      return E({Pexp_assert{std::move(arg)}, span(position(t.start), ae)});
    }
    if (cur().kind == Kind::LAZY) {
      Token t = cur(); advance();
      ExprBox arg = parse_atom_postfix();
      Position ae = arg->loc.end;
      return E({Pexp_lazy{std::move(arg)}, span(position(t.start), ae)});
    }
    // constructor application  Constr arg  -> Pexp_construct (not Pexp_apply)
    if (cur().kind == Kind::UIDENT) {
      PathResult pr = parse_dotted_path();
      if (pr.final_upper) {
        if (is_atom_start(cur().kind)) {
          ExprBox arg = parse_atom_postfix();
          Location l = span(pr.lid.loc.start, arg->loc.end);
          return collect_app(mk_construct(pr.lid, std::move(arg), l));
        }
        return collect_app(mk_construct(pr.lid, std::nullopt, pr.lid.loc));
      }
      Location l = pr.lid.loc;
      return collect_app(postfix_field(E({Pexp_ident{.id = std::move(pr.lid)}, l})));
    }
    if (cur().kind == Kind::TRUE || cur().kind == Kind::FALSE) {
      Token t = cur();
      advance();
      LongidentLoc cl = lid0(t.kind == Kind::TRUE ? "true" : "false", tokloc(t));
      return collect_app(mk_construct(cl, std::nullopt, cl.loc));
    }
    return collect_app(parse_atom_postfix());
  }

  ExprBox parse_unary() {
    Token t = cur();
    if (t.kind == Kind::MINUS || t.kind == Kind::MINUSDOT) {
      // negative literal: fold `- <int/float literal>` into a signed constant.
      if (peek(1).kind == Kind::INT || peek(1).kind == Kind::FLOAT) {
        advance();
        Token lit = cur(); advance();
        Location cl = span(position(t.start), position(lit.end));
        Constant c = (lit.kind == Kind::INT)
                         ? Constant{Pconst_integer{"-" + lit.text, lit.modifier}, cl}
                         : Constant{Pconst_float{"-" + lit.text, lit.modifier}, cl};
        return E({Pexp_constant{std::move(c)}, cl});
      }
      // otherwise `- e` applies ~- / ~-. to an application
      advance();
      ExprBox arg = parse_app();
      Position ae = arg->loc.end;
      ExprBox fn = ident_expr(t.kind == Kind::MINUS ? "~-" : "~-.", tokloc(t));
      std::vector<std::pair<ArgLabel, ExprBox>> args;
      args.emplace_back(Nolabel{}, std::move(arg));
      return E({Pexp_apply{std::move(fn), std::move(args)}, span(position(t.start), ae)});
    }
    return parse_app();
  }

  ExprBox parse_binop(int min_prec) {
    ExprBox left = parse_unary();
    for (;;) {
      // cons '::' is right-assoc at level 6 and builds a construct, not an apply.
      if (cur().kind == Kind::COLONCOLON && 6 >= min_prec) {
        Token optok = cur();
        advance();
        ExprBox right = parse_binop(6);
        Position ls = left->loc.start, re = right->loc.end;
        Location gl = gloc(ls, re);
        std::vector<ExprBox> tup;
        tup.push_back(std::move(left));
        tup.push_back(std::move(right));
        ExprBox tuple = E({Pexp_tuple{std::move(tup)}, gl});
        left = mk_construct(lid0("::", tokloc(optok)), std::move(tuple), Location{ls, re, false});
        continue;
      }
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

  static bool expr_starts(Kind k) {
    if (is_atom_start(k)) return true;
    switch (k) {
      case Kind::LET: case Kind::IF: case Kind::MATCH: case Kind::FUNCTION:
      case Kind::TRY: case Kind::FUN: case Kind::WHILE: case Kind::FOR:
      case Kind::ASSERT: case Kind::LAZY: case Kind::MINUS: case Kind::MINUSDOT:
        return true;
      default: return false;
    }
  }

  Case parse_case() {
    Pattern p = parse_pattern();
    std::optional<ExprBox> guard;
    if (cur().kind == Kind::WHEN) { advance(); guard = parse_expr(); }
    expect(Kind::MINUSGREATER, "->");
    ExprBox rhs = parse_expr();
    return Case{std::move(p), std::move(guard), std::move(rhs)};
  }
  std::vector<Case> parse_cases() {
    std::vector<Case> cs;
    if (cur().kind == Kind::BAR) advance();
    cs.push_back(parse_case());
    while (cur().kind == Kind::BAR) { advance(); cs.push_back(parse_case()); }
    return cs;
  }

  ExprBox parse_expr() {
    ExprBox e = parse_expr_no_seq();
    if (cur().kind == Kind::SEMI && expr_starts(peek(1).kind)) {
      advance();
      ExprBox e2 = parse_expr();
      Location l = span(e->loc.start, e2->loc.end);
      return E({Pexp_sequence{std::move(e), std::move(e2)}, l});
    }
    return e;
  }

  ExprBox parse_expr_no_seq() {
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
        ExprBox th = parse_expr_no_seq();
        std::optional<ExprBox> el;
        if (cur().kind == Kind::ELSE) { advance(); el = parse_expr_no_seq(); }
        Position end = el ? (*el)->loc.end : th->loc.end;
        Location l = span(position(t.start), end);
        return E({Pexp_ifthenelse{std::move(c), std::move(th), std::move(el)}, l});
      }
      case Kind::MATCH: {
        advance();
        ExprBox e0 = parse_expr();
        expect(Kind::WITH, "with");
        std::vector<Case> cs = parse_cases();
        Location l = span(position(t.start), cs.back().rhs->loc.end);
        return E({Pexp_match{std::move(e0), std::move(cs)}, l});
      }
      case Kind::TRY: {
        advance();
        ExprBox e0 = parse_expr();
        expect(Kind::WITH, "with");
        std::vector<Case> cs = parse_cases();
        Location l = span(position(t.start), cs.back().rhs->loc.end);
        return E({Pexp_try{std::move(e0), std::move(cs)}, l});
      }
      case Kind::FUNCTION: {
        advance();
        std::vector<Case> cs = parse_cases();
        Position last = cs.back().rhs->loc.end;
        Location casesloc = span(position(t.start), last);
        auto fb = box(FunctionBody{Pfunction_cases{std::move(cs), casesloc}});
        return E({Pexp_function{{}, std::move(fb)}, span(position(t.start), last)});
      }
      case Kind::FUN: {
        advance();
        std::vector<FunctionParam> params;
        while (cur().kind != Kind::MINUSGREATER) params.push_back(parse_param());
        expect(Kind::MINUSGREATER, "->");
        ExprBox body = parse_expr();
        Location l = span(position(t.start), body->loc.end);
        auto fb = box(FunctionBody{Pfunction_body{std::move(body)}});
        return E({Pexp_function{std::move(params), std::move(fb)}, l});
      }
      case Kind::WHILE: {
        advance();
        ExprBox cond = parse_expr();
        expect(Kind::DO, "do");
        ExprBox body = parse_expr();
        Token c = cur(); expect(Kind::DONE, "done");
        return E({Pexp_while{std::move(cond), std::move(body)},
                  span(position(t.start), position(c.end))});
      }
      case Kind::FOR: {
        advance();
        Pattern var = parse_simple_pattern();
        expect(Kind::EQUAL, "=");
        ExprBox lo = parse_expr();
        DirectionFlag dir;
        if (cur().kind == Kind::TO) { dir = DirectionFlag::Upto; advance(); }
        else if (cur().kind == Kind::DOWNTO) { dir = DirectionFlag::Downto; advance(); }
        else throw ParseError("expected 'to' or 'downto'", cur().start);
        ExprBox hi = parse_expr();
        expect(Kind::DO, "do");
        ExprBox body = parse_expr();
        Token c = cur(); expect(Kind::DONE, "done");
        return E({Pexp_for{std::move(var), std::move(lo), std::move(hi), dir, std::move(body)},
                  span(position(t.start), position(c.end))});
      }
      default:
        return parse_tuple();
    }
  }

  // ---- longidents ----
  // A dotted path of identifiers; the kinds allowed at each step are the
  // caller's concern (value path ends in lident, module/constr path in uident).
  LongidentLoc parse_longident_path() {
    Token first = cur();
    advance();
    Longident lid{Lident{first.text}};
    Token last = first;
    while (cur().kind == Kind::DOT &&
           (peek(1).kind == Kind::LIDENT || peek(1).kind == Kind::UIDENT)) {
      advance();
      Token nm = cur();
      advance();
      lid = Longident{Ldot{std::make_shared<Longident>(std::move(lid)), nm.text}};
      last = nm;
    }
    return LongidentLoc{std::move(lid), span(position(first.start), position(last.end))};
  }

  // ---- core types ----
  CoreTypeBox parse_core_type() { return parse_type_arrow(); }
  CoreTypeBox parse_type_arrow() {
    CoreTypeBox t = parse_type_tuple();
    if (cur().kind == Kind::MINUSGREATER) {
      advance();
      CoreTypeBox cod = parse_type_arrow();
      Location l = span(t->loc.start, cod->loc.end);
      return box(CoreType{Ptyp_arrow{Nolabel{}, std::move(t), std::move(cod)}, l});
    }
    return t;
  }
  CoreTypeBox parse_type_tuple() {
    CoreTypeBox t = parse_type_app();
    if (cur().kind != Kind::STAR) return t;
    std::vector<CoreTypeBox> elems;
    Position s = t->loc.start;
    elems.push_back(std::move(t));
    while (cur().kind == Kind::STAR) { advance(); elems.push_back(parse_type_app()); }
    Location l = span(s, elems.back()->loc.end);
    return box(CoreType{Ptyp_tuple{std::move(elems)}, l});
  }
  CoreTypeBox parse_type_app() {
    CoreTypeBox t = parse_type_atom();
    while (cur().kind == Kind::LIDENT || cur().kind == Kind::UIDENT) {
      LongidentLoc name = parse_longident_path();
      std::vector<CoreTypeBox> args;
      Position s = t->loc.start;
      args.push_back(std::move(t));
      t = box(CoreType{.desc = Ptyp_constr{.id = name, .args = std::move(args)}, .loc = span(s, name.loc.end)});
    }
    return t;
  }
  CoreTypeBox parse_type_atom() {
    Token t = cur();
    if (t.kind == Kind::UNDERSCORE) { advance(); return box(CoreType{Ptyp_any{}, tokloc(t)}); }
    if (t.kind == Kind::QUOTE) {
      advance();
      Token nm = cur();
      if (nm.kind != Kind::LIDENT) throw ParseError("expected type variable", nm.start);
      advance();
      return box(CoreType{Ptyp_var{nm.text}, span(position(t.start), position(nm.end))});
    }
    if (t.kind == Kind::LIDENT || t.kind == Kind::UIDENT) {
      LongidentLoc name = parse_longident_path();
      return box(CoreType{.desc = Ptyp_constr{.id = name, .args = {}}, .loc = name.loc});
    }
    if (t.kind == Kind::LPAREN) {
      advance();
      CoreTypeBox inner = parse_core_type();
      if (cur().kind == Kind::COMMA) {
        std::vector<CoreTypeBox> args;
        args.push_back(std::move(inner));
        while (cur().kind == Kind::COMMA) { advance(); args.push_back(parse_core_type()); }
        expect(Kind::RPAREN, ")");
        LongidentLoc name = parse_longident_path();
        return box(CoreType{.desc = Ptyp_constr{.id = name, .args = std::move(args)},
                            .loc = span(position(t.start), name.loc.end)});
      }
      Token close = cur();
      expect(Kind::RPAREN, ")");
      inner->loc = span(position(t.start), position(close.end));
      return inner;
    }
    throw ParseError("expected a type", t.start);
  }

  // ---- patterns ----
  static bool is_simple_pattern_start(Kind k) {
    switch (k) {
      case Kind::LIDENT: case Kind::UNDERSCORE: case Kind::LPAREN: case Kind::INT:
      case Kind::FLOAT: case Kind::CHAR: case Kind::STRING: case Kind::UIDENT:
      case Kind::TRUE: case Kind::FALSE: case Kind::LBRACKET: case Kind::LBRACE:
        return true;
      default: return false;
    }
  }
  Pattern parse_pattern() { return parse_pat_alias(); }
  Pattern parse_pat_alias() {
    Pattern p = parse_pat_or();
    while (cur().kind == Kind::AS) {
      advance();
      Token nm = cur();
      if (nm.kind != Kind::LIDENT) throw ParseError("expected name after 'as'", nm.start);
      advance();
      Location l = span(p.loc.start, position(nm.end));
      p = Pattern{Ppat_alias{box(std::move(p)), StringLoc{nm.text, tokloc(nm)}}, l};
    }
    return p;
  }
  Pattern parse_pat_or() {
    Pattern p = parse_pat_tuple();
    while (cur().kind == Kind::BAR) {
      advance();
      Pattern r = parse_pat_tuple();
      Location l = span(p.loc.start, r.loc.end);
      p = Pattern{Ppat_or{box(std::move(p)), box(std::move(r))}, l};
    }
    return p;
  }
  Pattern parse_pat_tuple() {
    Pattern p = parse_pat_cons();
    if (cur().kind != Kind::COMMA) return p;
    std::vector<PatBox> elems;
    Position s = p.loc.start;
    elems.push_back(box(std::move(p)));
    while (cur().kind == Kind::COMMA) { advance(); elems.push_back(box(parse_pat_cons())); }
    Location l = span(s, elems.back()->loc.end);
    return Pattern{Ppat_tuple{std::move(elems), ClosedFlag::Closed}, l};
  }
  Pattern parse_pat_cons() {
    Pattern p = parse_pat_app();
    if (cur().kind != Kind::COLONCOLON) return p;
    Token optok = cur();
    advance();
    Pattern r = parse_pat_cons();  // right-assoc
    Position ls = p.loc.start, re = r.loc.end;
    Location gl = gloc(ls, re);
    std::vector<PatBox> tup;
    tup.push_back(box(std::move(p)));
    tup.push_back(box(std::move(r)));
    Pattern tuple{Ppat_tuple{std::move(tup), ClosedFlag::Closed}, gl};
    return Pattern{Ppat_construct{.id = lid0("::", tokloc(optok)), .arg = box(std::move(tuple))},
                   Location{ls, re, false}};
  }
  Pattern parse_pat_app() {
    if (cur().kind == Kind::UIDENT) {
      LongidentLoc cl = parse_longident_path();
      if (is_simple_pattern_start(cur().kind)) {
        Pattern arg = parse_simple_pattern();
        Location l = span(cl.loc.start, arg.loc.end);
        return Pattern{Ppat_construct{.id = cl, .arg = box(std::move(arg))}, l};
      }
      return Pattern{Ppat_construct{.id = cl, .arg = std::nullopt}, cl.loc};
    }
    return parse_simple_pattern();
  }
  Pattern ppat_construct0(const char* name, Location l) {
    return Pattern{Ppat_construct{.id = LongidentLoc{.txt = {Lident{name}}, .loc = l}, .arg = std::nullopt}, l};
  }
  Pattern build_pat_list(std::vector<Pattern>& elems, Position lb, Position rbS, Position rbE) {
    Pattern acc = ppat_construct0("[]", gloc(rbS, rbE));
    for (int i = static_cast<int>(elems.size()) - 1; i >= 0; --i) {
      Position es = elems[i].loc.start;
      Location gl = gloc(es, rbE);
      std::vector<PatBox> tup;
      tup.push_back(box(std::move(elems[i])));
      tup.push_back(box(std::move(acc)));
      Pattern tuple{Ppat_tuple{std::move(tup), ClosedFlag::Closed}, gl};
      acc = Pattern{Ppat_construct{.id = lid0("::", gl), .arg = box(std::move(tuple))}, gl};
    }
    acc.loc = Location{lb, rbE, false};
    return acc;
  }
  Pattern parse_simple_pattern() {
    Token t = cur();
    switch (t.kind) {
      case Kind::UNDERSCORE: advance(); return {Ppat_any{}, tokloc(t)};
      case Kind::LIDENT: advance(); return {Ppat_var{StringLoc{t.text, tokloc(t)}}, tokloc(t)};
      case Kind::INT: case Kind::FLOAT: case Kind::CHAR: case Kind::STRING:
        advance(); return {Ppat_constant{const_of(t)}, tokloc(t)};
      case Kind::TRUE: advance(); return ppat_construct0("true", tokloc(t));
      case Kind::FALSE: advance(); return ppat_construct0("false", tokloc(t));
      case Kind::UIDENT: { LongidentLoc cl = parse_longident_path();
        return {Ppat_construct{.id = cl, .arg = std::nullopt}, cl.loc}; }
      case Kind::LPAREN: {
        advance();
        if (cur().kind == Kind::RPAREN) {
          Token c = cur(); advance();
          return ppat_construct0("()", span(position(t.start), position(c.end)));
        }
        Pattern p = parse_pattern();
        if (cur().kind == Kind::COLON) {
          advance();
          CoreTypeBox ty = parse_core_type();
          Token c = cur(); expect(Kind::RPAREN, ")");
          return {Ppat_constraint{box(std::move(p)), std::move(ty)},
                  span(position(t.start), position(c.end))};
        }
        Token c = cur(); expect(Kind::RPAREN, ")");
        p.loc = span(position(t.start), position(c.end));  // reloc to parens
        return p;
      }
      case Kind::LBRACKET: {
        advance();
        if (cur().kind == Kind::RBRACKET) {
          Token c = cur(); advance();
          return ppat_construct0("[]", span(position(t.start), position(c.end)));
        }
        std::vector<Pattern> elems;
        elems.push_back(parse_pattern());
        while (cur().kind == Kind::SEMI) {
          advance();
          if (cur().kind == Kind::RBRACKET) break;
          elems.push_back(parse_pattern());
        }
        Token c = cur(); expect(Kind::RBRACKET, "]");
        return build_pat_list(elems, position(t.start), position(c.start), position(c.end));
      }
      case Kind::LBRACE: {
        advance();
        std::vector<std::pair<LongidentLoc, PatBox>> fields;
        ClosedFlag closed = ClosedFlag::Closed;
        while (cur().kind != Kind::RBRACE) {
          if (cur().kind == Kind::UNDERSCORE) {
            advance(); closed = ClosedFlag::Open;
            if (cur().kind == Kind::SEMI) advance();
            break;
          }
          LongidentLoc lbl = parse_longident_path();
          if (cur().kind == Kind::EQUAL) {
            advance();
            fields.emplace_back(lbl, box(parse_pattern()));
          } else {
            // punning { x }: the label longident is ghost; the var pattern is real
            LongidentLoc glbl = lbl;
            glbl.loc.ghost = true;
            fields.emplace_back(glbl,
                box(Pattern{Ppat_var{StringLoc{lid_last_name(lbl.txt), lbl.loc}}, lbl.loc}));
          }
          if (cur().kind == Kind::SEMI) advance(); else break;
        }
        Token c = cur(); expect(Kind::RBRACE, "}");
        return {Ppat_record{std::move(fields), closed}, span(position(t.start), position(c.end))};
      }
      default: throw ParseError("unsupported pattern", t.start);
    }
  }

  // ---- type declarations ----
  std::vector<LabelDecl> parse_label_decls() {
    std::vector<LabelDecl> fields;
    expect(Kind::LBRACE, "{");
    while (cur().kind != Kind::RBRACE) {
      Token start = cur();
      MutableFlag mut = MutableFlag::Immutable;
      if (cur().kind == Kind::MUTABLE) { advance(); mut = MutableFlag::Mutable; }
      Token nm = cur();
      if (nm.kind != Kind::LIDENT) throw ParseError("expected field name", nm.start);
      advance();
      expect(Kind::COLON, ":");
      CoreTypeBox ty = parse_core_type();
      // pld_loc includes the trailing ';' separator when present.
      Position endp = ty->loc.end;
      bool more = false;
      if (cur().kind == Kind::SEMI) { endp = position(cur().end); advance(); more = true; }
      fields.push_back(LabelDecl{StringLoc{nm.text, tokloc(nm)}, mut, std::move(ty),
                                 span(position(start.start), endp)});
      if (!more) break;
    }
    expect(Kind::RBRACE, "}");
    return fields;
  }
  ConstructorDecl parse_constructor_decl(Position start) {
    Token nm = cur();
    if (nm.kind != Kind::UIDENT) throw ParseError("expected constructor name", nm.start);
    advance();
    ConstructorArguments args = Pcstr_tuple{};
    Position endp = position(nm.end);
    if (cur().kind == Kind::OF) {
      advance();
      if (cur().kind == Kind::LBRACE) {
        auto fs = parse_label_decls();
        endp = position(tokens_[idx_ - 1].end);
        args = Pcstr_record{std::move(fs)};
      } else {
        std::vector<CoreTypeBox> ts;
        ts.push_back(parse_type_app());
        while (cur().kind == Kind::STAR) { advance(); ts.push_back(parse_type_app()); }
        endp = ts.back()->loc.end;
        args = Pcstr_tuple{std::move(ts)};
      }
    }
    return ConstructorDecl{StringLoc{nm.text, tokloc(nm)}, std::move(args), std::nullopt,
                           span(start, endp)};
  }
  TypeDeclaration parse_type_declaration(Position declStart) {
    std::vector<CoreTypeBox> params;
    if (cur().kind == Kind::QUOTE) params.push_back(parse_type_atom());
    else if (cur().kind == Kind::LPAREN) {
      advance();
      params.push_back(parse_core_type());
      while (cur().kind == Kind::COMMA) { advance(); params.push_back(parse_core_type()); }
      expect(Kind::RPAREN, ")");
    }
    Token nm = cur();
    if (nm.kind != Kind::LIDENT) throw ParseError("expected type name", nm.start);
    advance();
    TypeKind kind = Ptype_abstract{};
    std::optional<CoreTypeBox> manifest;
    PrivateFlag priv = PrivateFlag::Public;
    if (cur().kind == Kind::EQUAL) {
      advance();
      if (cur().kind == Kind::PRIVATE) { advance(); priv = PrivateFlag::Private; }
      if (cur().kind == Kind::LBRACE) {
        kind = Ptype_record{parse_label_decls()};
      } else if (cur().kind == Kind::BAR || cur().kind == Kind::UIDENT) {
        // a constructor_declaration's loc includes its leading '|' (if any)
        Position cs = position(cur().start);
        if (cur().kind == Kind::BAR) advance();
        std::vector<ConstructorDecl> ctors;
        ctors.push_back(parse_constructor_decl(cs));
        while (cur().kind == Kind::BAR) {
          Position bs = position(cur().start);
          advance();
          ctors.push_back(parse_constructor_decl(bs));
        }
        kind = Ptype_variant{std::move(ctors)};
      } else {
        manifest = parse_core_type();
      }
    }
    Location l = span(declStart, position(tokens_[idx_ - 1].end));
    return TypeDeclaration{StringLoc{nm.text, tokloc(nm)}, std::move(params),
                           std::move(kind), priv, std::move(manifest), l};
  }

  // ---- bindings ----
  FunctionParam parse_param() {
    Token t = cur();
    // ~x  (labelled punning)
    if (t.kind == Kind::TILDE && peek(1).kind == Kind::LIDENT) {
      advance(); Token id = cur(); advance();
      Pattern p{Ppat_var{StringLoc{id.text, tokloc(id)}}, tokloc(id)};
      Location loc = span(position(t.start), position(id.end));
      return FunctionParam{Pparam_val{loc, Labelled{id.text}, std::nullopt, std::move(p)}};
    }
    // ?x  (optional punning)
    if (t.kind == Kind::QUESTION && peek(1).kind == Kind::LIDENT) {
      advance(); Token id = cur(); advance();
      Pattern p{Ppat_var{StringLoc{id.text, tokloc(id)}}, tokloc(id)};
      Location loc = span(position(t.start), position(id.end));
      return FunctionParam{Pparam_val{loc, Optional{id.text}, std::nullopt, std::move(p)}};
    }
    // ?(x [: t] = e)  optional with default
    if (t.kind == Kind::QUESTION && peek(1).kind == Kind::LPAREN) {
      advance(); advance();  // ? (
      Token id = cur();
      if (id.kind != Kind::LIDENT) throw ParseError("expected label name", id.start);
      advance();
      Pattern p{Ppat_var{StringLoc{id.text, tokloc(id)}}, tokloc(id)};
      if (cur().kind == Kind::COLON) {
        advance(); CoreTypeBox ty = parse_core_type();
        Location pl = p.loc;
        p = Pattern{Ppat_constraint{box(std::move(p)), std::move(ty)}, pl};
      }
      std::optional<ExprBox> def;
      if (cur().kind == Kind::EQUAL) { advance(); def = parse_expr(); }
      Token c = cur(); expect(Kind::RPAREN, ")");
      Location loc = span(position(t.start), position(c.end));
      return FunctionParam{Pparam_val{loc, Optional{id.text}, std::move(def), std::move(p)}};
    }
    Pattern p = parse_simple_pattern();
    Location l = p.loc;
    return FunctionParam{Pparam_val{l, Nolabel{}, std::nullopt, std::move(p)}};
  }

  ValueBinding parse_value_binding() {
    // val_ident form (`let f p.. = e`) vs pattern form (`let pat = e`).
    bool val_ident = cur().kind == Kind::LIDENT &&
                     (peek(1).kind == Kind::EQUAL || peek(1).kind == Kind::COLON ||
                      is_simple_pattern_start(peek(1).kind));
    if (!val_ident) {
      Pattern pat = parse_pattern();
      expect(Kind::EQUAL, "=");
      ExprBox body = parse_expr();
      return ValueBinding{std::move(pat), std::move(body), std::nullopt};
    }
    Token nt = cur();
    advance();
    Location nl = tokloc(nt);
    Pattern namepat{Ppat_var{StringLoc{nt.text, nl}}, nl};

    std::vector<FunctionParam> params;
    while (cur().kind != Kind::EQUAL && cur().kind != Kind::COLON)
      params.push_back(parse_param());
    std::optional<CoreTypeBox> constr;
    if (cur().kind == Kind::COLON) {
      // `let x : t = e`  (simple value constraint). The function-with-return-type
      // form (`let f x : t = e`) lowers differently; defer it.
      if (!params.empty())
        throw ParseError("constrained function binding not supported yet", cur().start);
      advance();
      constr = parse_core_type();
    }
    expect(Kind::EQUAL, "=");
    ExprBox body = parse_expr();

    if (params.empty())
      return ValueBinding{std::move(namepat), std::move(body), std::move(constr)};
    // desugar `let f p.. = e` to a ghost Pexp_function spanning p[0]..e
    Position fstart = std::get<Pparam_val>(params.front().desc).loc.start;
    Location floc = span(fstart, body->loc.end, /*ghost=*/true);
    auto fb = box(FunctionBody{Pfunction_body{std::move(body)}});
    ExprBox fn = E({Pexp_function{std::move(params), std::move(fb)}, floc});
    return ValueBinding{std::move(namepat), std::move(fn), std::nullopt};
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
    if (t.kind == Kind::TYPE) {
      advance();
      RecFlag rf = RecFlag::Recursive;  // `type` is recursive by default
      if (cur().kind == Kind::NONREC) { advance(); rf = RecFlag::Nonrecursive; }
      std::vector<TypeDeclaration> decls;
      decls.push_back(parse_type_declaration(position(t.start)));  // first decl: from `type`
      while (cur().kind == Kind::AND) {
        Position ds = position(cur().start);  // subsequent decls: from `and`
        advance();
        decls.push_back(parse_type_declaration(ds));
      }
      Location l = span(position(t.start), position(tokens_[idx_ - 1].end));
      return StructureItem{Pstr_type{rf, std::move(decls)}, l};
    }
    if (t.kind == Kind::OPEN) {
      advance();
      OverrideFlag ovr = OverrideFlag::Fresh;
      if (cur().kind == Kind::BANG) { advance(); ovr = OverrideFlag::Override; }
      LongidentLoc mp = parse_longident_path();
      ModuleExpr me{.desc = Pmod_ident{.id = mp}, .loc = mp.loc};
      Location l = span(position(t.start), position(tokens_[idx_ - 1].end));
      return StructureItem{Pstr_open{ovr, std::move(me)}, l};
    }
    ExprBox e = parse_expr();
    Location l = e->loc;
    return StructureItem{Pstr_eval{std::move(e)}, l};
  }
};

}  // namespace

Structure parse_structure(std::string_view src) { return Parser(src).parse_structure(); }

}  // namespace cppcaml
