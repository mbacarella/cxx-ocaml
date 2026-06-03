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
    // note: `<-` (LESSMINUS) is assignment, not an operator — handled in parse_binop
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

// `( operator )` as a value identifier: maps the operator token to its name,
// or returns nullopt if this token can't begin a parenthesized operator.
std::optional<std::string> operator_name(const Token& t) {
  switch (t.kind) {
    case Kind::PREFIXOP: case Kind::HASHOP: case Kind::LETOP: case Kind::ANDOP:
    case Kind::INFIXOP0: case Kind::INFIXOP1: case Kind::INFIXOP2:
    case Kind::INFIXOP3: case Kind::INFIXOP4:
      return t.text;
    case Kind::BANG: return "!";
    case Kind::PLUS: return "+";
    case Kind::PLUSDOT: return "+.";
    case Kind::PLUSEQ: return "+=";
    case Kind::MINUS: return "-";
    case Kind::MINUSDOT: return "-.";
    case Kind::STAR: return "*";
    case Kind::PERCENT: return "%";
    case Kind::EQUAL: return "=";
    case Kind::LESS: return "<";
    case Kind::GREATER: return ">";
    case Kind::OR: return "or";
    case Kind::BARBAR: return "||";
    case Kind::AMPERSAND: return "&";
    case Kind::AMPERAMPER: return "&&";
    case Kind::COLONEQUAL: return ":=";
    default: return std::nullopt;
  }
}

bool is_atom_start(Kind k) {
  switch (k) {
    case Kind::INT: case Kind::FLOAT: case Kind::CHAR: case Kind::STRING:
    case Kind::LIDENT: case Kind::UIDENT: case Kind::LPAREN:
    case Kind::TRUE: case Kind::FALSE: case Kind::LBRACKET: case Kind::LBRACE:
    case Kind::BANG: case Kind::PREFIXOP: case Kind::LBRACKETBAR:
    case Kind::BEGIN: case Kind::LBRACKETPERCENT: case Kind::BACKQUOTE:
    case Kind::OBJECT: case Kind::NEW: case Kind::LBRACELESS:
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

  Structure parse_structure() { return parse_structure_until(Kind::TEOF); }
  Structure parse_structure_until(Kind stop) {
    Structure items;
    while (cur().kind != Kind::TEOF && cur().kind != stop) {
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
      default: {  // STRING: strloc is the *content* span (inside the quotes/delimiters)
        size_t d = t.delim ? t.delim->size() + 2 : 1;  // {delim| … |delim}  or  " … "
        Location strloc{position(t.start + d), position(t.end - d), false};
        return Constant{Pconst_string{t.text, strloc, t.delim}, l};
      }
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

  // M.(e) / M.[…] / M.{…} / M.[|…|]  -> Pexp_struct_item over a ghost Pstr_open,
  // except M.(op) which is just the qualified value identifier "M.op".
  ExprBox make_local_open(const LongidentLoc& modpath, ExprBox body, Location whole) {
    ModuleExpr me{Pmod_ident{modpath}, modpath.loc};
    StructureItem si{Pstr_open{OverrideFlag::Fresh, std::move(me)}, none_loc()};
    return E({Pexp_struct_item{box(std::move(si)), std::move(body)}, whole});
  }
  std::optional<ExprBox> try_local_open(const PathResult& pr) {
    if (!pr.final_upper || cur().kind != Kind::DOT) return std::nullopt;
    Kind k = peek(1).kind;
    if (k != Kind::LPAREN && k != Kind::LBRACKET && k != Kind::LBRACE && k != Kind::LBRACKETBAR)
      return std::nullopt;
    advance();  // '.'
    Position openStart = pr.lid.loc.start;
    if (cur().kind == Kind::LPAREN) {
      Token lp = cur();
      if (operator_name(peek(1)) && peek(2).kind == Kind::RPAREN) {  // M.(op) -> "M.op"
        advance();  // (
        auto op = operator_name(cur()); advance();
        Token c = cur(); advance();  // )
        Longident qual{Ldot{std::make_shared<Longident>(pr.lid.txt), *op}};
        Location l = span(openStart, position(c.end));
        return E({Pexp_ident{LongidentLoc{std::move(qual), l}}, l});
      }
      advance();  // (
      ExprBox inner;
      if (cur().kind == Kind::RPAREN) {  // M.()
        Location ul = span(position(lp.start), position(cur().end));
        inner = mk_construct(lid0("()", ul), std::nullopt, ul);
      } else {
        inner = parse_expr();
      }
      Token c = cur(); expect(Kind::RPAREN, ")");
      return make_local_open(pr.lid, std::move(inner), span(openStart, position(c.end)));
    }
    ExprBox inner = parse_atom();  // M.[…] / M.{…} / M.[|…|]
    Position end = inner->loc.end;
    return make_local_open(pr.lid, std::move(inner), span(openStart, end));
  }

  // postfix record-field access  e.lbl (.lbl)*
  ExprBox qualified_ident(const char* mod, const char* fn, Location l) {
    Longident lid{Ldot{std::make_shared<Longident>(Longident{Lident{mod}}), fn}};
    return E({Pexp_ident{.id = LongidentLoc{std::move(lid), l}}, l});
  }
  LongidentLoc lid_of_dotted(const std::string& s, Location l) {
    std::vector<std::string> parts;
    size_t start = 0;
    for (size_t i = 0; i <= s.size(); ++i)
      if (i == s.size() || s[i] == '.') { parts.push_back(s.substr(start, i - start)); start = i + 1; }
    Longident lid{Lident{parts[0]}};
    for (size_t i = 1; i < parts.size(); ++i)
      lid = Longident{Ldot{std::make_shared<Longident>(std::move(lid)), parts[i]}};
    return LongidentLoc{std::move(lid), l};
  }
  // a.{i[,j[,k]]}  ->  Bigarray.ArrayN.get a i [j [k]]  (Genarray for >3 indices)
  ExprBox bigarray_get(ExprBox e, std::vector<ExprBox> idxs, Position end) {
    Position s = e->loc.start;
    Location gl{s, end, true};
    int n = static_cast<int>(idxs.size());
    const char* fn = n == 1 ? "Bigarray.Array1.get" : n == 2 ? "Bigarray.Array2.get"
                   : n == 3 ? "Bigarray.Array3.get" : "Bigarray.Genarray.get";
    ExprBox fnexpr = E({Pexp_ident{lid_of_dotted(fn, gl)}, gl});
    std::vector<std::pair<ArgLabel, ExprBox>> args;
    args.emplace_back(Nolabel{}, std::move(e));
    if (n <= 3) {
      for (auto& ix : idxs) args.emplace_back(Nolabel{}, std::move(ix));
    } else {
      args.emplace_back(Nolabel{}, E({Pexp_array{std::move(idxs)}, gl}));
    }
    return E({Pexp_apply{std::move(fnexpr), std::move(args)}, Location{s, end, false}});
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
      if (cur().kind == Kind::DOT &&
          (peek(1).kind == Kind::LIDENT || peek(1).kind == Kind::UIDENT)) {
        advance();  // .  (field may be a qualified path  e.M.f)
        LongidentLoc field = parse_longident_path();
        Location l = span(e->loc.start, field.loc.end);
        e = E({Pexp_field{std::move(e), std::move(field)}, l});
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
      } else if (cur().kind == Kind::DOT && peek(1).kind == Kind::LBRACE) {
        advance(); advance();  // . {   (bigarray indexing; commas separate indices)
        std::vector<ExprBox> idxs;
        idxs.push_back(parse_binop(0));
        while (cur().kind == Kind::COMMA) { advance(); idxs.push_back(parse_binop(0)); }
        Token c = cur(); expect(Kind::RBRACE, "}");
        e = bigarray_get(std::move(e), std::move(idxs), position(c.end));
      } else if (cur().kind == Kind::HASH && peek(1).kind == Kind::LIDENT) {
        advance();  // #
        Token m = cur(); advance();
        Location l = span(e->loc.start, position(m.end));
        e = E({Pexp_send{std::move(e), StringLoc{m.text, tokloc(m)}}, l});
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
        if (auto lo = try_local_open(pr)) return std::move(*lo);
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
        if (cur().kind == Kind::MODULE) {  // (module ME [: S])  first-class module
          advance();
          ModuleExpr me = parse_module_expr();
          if (cur().kind == Kind::COLON) { advance(); parse_module_type(); }  // package type discarded
          Token c = cur(); expect(Kind::RPAREN, ")");
          return E({Pexp_pack{box(std::move(me))}, span(position(t.start), position(c.end))});
        }
        if (peek(1).kind == Kind::RPAREN) {
          if (auto op = operator_name(cur())) {  // (+), (>>=), (!), …  -> Pexp_ident
            advance();
            Token c = cur(); advance();  // RPAREN
            Location l = span(position(t.start), position(c.end));  // spans the parens
            return E({Pexp_ident{lid0(*op, l)}, l});
          }
        }
        ExprBox inner = parse_expr();
        if (cur().kind == Kind::COLON) {
          advance();
          CoreTypeBox ty = parse_core_type();
          if (cur().kind == Kind::COLONGREATER) {  // (e : t :> t2)
            advance();
            CoreTypeBox ty2 = parse_core_type();
            Token c = cur(); expect(Kind::RPAREN, ")");
            return E({Pexp_coerce{std::move(inner), std::move(ty), std::move(ty2)},
                      span(position(t.start), position(c.end))});
          }
          Token c = cur(); expect(Kind::RPAREN, ")");
          return E({Pexp_constraint{std::move(inner), std::move(ty)},
                    span(position(t.start), position(c.end))});
        }
        if (cur().kind == Kind::COLONGREATER) {  // (e :> t)
          advance();
          CoreTypeBox ty2 = parse_core_type();
          Token c = cur(); expect(Kind::RPAREN, ")");
          return E({Pexp_coerce{std::move(inner), std::nullopt, std::move(ty2)},
                    span(position(t.start), position(c.end))});
        }
        Token c = cur(); expect(Kind::RPAREN, ")");
        inner->loc = span(position(t.start), position(c.end));  // reloc to parens
        return inner;
      }
      case Kind::LBRACKETPERCENT: {  // [%id payload]
        advance();
        std::string name = parse_attr_name();
        Structure payload = parse_structure_until(Kind::RBRACKET);
        Token c = cur(); expect(Kind::RBRACKET, "]");
        return E({Pexp_extension{std::move(name), std::move(payload)},
                  span(position(t.start), position(c.end))});
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
          if (cur().kind == Kind::EQUAL) {
            advance();
            fields.emplace_back(lbl, parse_expr_no_seq());
          } else {  // punning { x }: the label longident is ghost, value ident real
            LongidentLoc glbl = lbl;
            glbl.loc.ghost = true;
            fields.emplace_back(glbl, E({Pexp_ident{.id = lbl}, lbl.loc}));
          }
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
      case Kind::OBJECT: {
        advance();
        ClassStructure cs = parse_class_structure_body();
        Token c = cur(); expect(Kind::END, "end");
        return E({Pexp_object{box(std::move(cs))}, span(position(t.start), position(c.end))});
      }
      case Kind::NEW: {
        advance();
        LongidentLoc id = parse_longident_path();
        return E({Pexp_new{id}, span(position(t.start), id.loc.end)});
      }
      case Kind::LBRACELESS: {  // {< field [= e]; … >}
        advance();
        std::vector<std::pair<StringLoc, ExprBox>> fields;
        while (cur().kind != Kind::GREATERRBRACE) {
          Token nm = cur(); advance();
          StringLoc name{nm.text, tokloc(nm)};
          if (cur().kind == Kind::EQUAL) { advance(); fields.emplace_back(name, parse_expr_no_seq()); }
          else fields.emplace_back(name, ident_expr(nm.text, name.loc));  // punning {< x >}
          if (cur().kind == Kind::SEMI) advance(); else break;
        }
        Token c = cur(); expect(Kind::GREATERRBRACE, ">}");
        return E({Pexp_override{std::move(fields)}, span(position(t.start), position(c.end))});
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

  ExprBox parse_atom_postfix() {
    ExprBox e = postfix_field(parse_atom());
    while (cur().kind == Kind::LBRACKETAT) {  // e [@attr]  -> pexp_attributes
      advance();
      e->attrs.push_back(parse_attribute_body());
    }
    return e;
  }

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
      if (auto lo = try_local_open(pr)) return collect_app(postfix_field(std::move(*lo)));
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
    if (cur().kind == Kind::BACKQUOTE) {  // polymorphic variant  `Tag [arg]
      Token t = cur(); advance();
      Token tag = cur(); advance();
      if (is_atom_start(cur().kind)) {
        ExprBox arg = parse_atom_postfix();
        Location l = span(position(t.start), arg->loc.end);
        return collect_app(E({Pexp_variant{tag.text, std::move(arg)}, l}));
      }
      return collect_app(
          E({Pexp_variant{tag.text, std::nullopt}, span(position(t.start), position(tag.end))}));
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

  // `lhs <- rhs`: e.l<-v -> Pexp_setfield; e.(i)<-v / e.[i]<-v -> Array/String.set;
  // x<-v (a bare ident) -> Pexp_setinstvar.
  ExprBox make_assignment(ExprBox lhs, ExprBox rhs, Location l) {
    if (auto* f = std::get_if<Pexp_field>(&lhs->desc)) {
      LongidentLoc field = f->field;
      ExprBox obj = std::move(f->e);
      return E({Pexp_setfield{std::move(obj), field, std::move(rhs)}, l});
    }
    if (auto* ap = std::get_if<Pexp_apply>(&lhs->desc)) {
      if (auto* id = std::get_if<Pexp_ident>(&ap->fn->desc)) {
        if (auto* lid = std::get_if<Lident>(&id->id.txt.v)) {
          (void)lid;
        }
        // Array.get / String.get -> .set with the rhs appended
        std::string nm;
        if (auto* dot = std::get_if<Ldot>(&id->id.txt.v)) nm = dot->name;
        if (nm == "get") {
          // rebuild fn as Mod.set (ghost over the whole assignment), append rhs
          Longident newlid{Ldot{std::make_shared<Longident>(*std::get_if<Ldot>(&id->id.txt.v)->prefix),
                                "set"}};
          Location gl{l.start, l.end, true};  // ghost ident spans `a.(i) <- v`
          ExprBox fn = E({Pexp_ident{.id = LongidentLoc{std::move(newlid), gl}}, gl});
          std::vector<std::pair<ArgLabel, ExprBox>> args = std::move(ap->args);
          args.emplace_back(Nolabel{}, std::move(rhs));
          return E({Pexp_apply{std::move(fn), std::move(args)}, l});
        }
      }
    }
    if (auto* id = std::get_if<Pexp_ident>(&lhs->desc)) {
      if (auto* lid = std::get_if<Lident>(&id->id.txt.v)) {
        return E({Pexp_setinstvar{StringLoc{lid->name, id->id.loc}, std::move(rhs)}, l});
      }
    }
    throw ParseError("invalid assignment target", lhs->loc.start.cnum);
  }
  ExprBox parse_binop(int min_prec) {
    ExprBox left = parse_unary();
    for (;;) {
      if (cur().kind == Kind::LESSMINUS && 1 >= min_prec) {  // assignment
        advance();
        Position ls = left->loc.start;
        ExprBox right = parse_binop(1);
        Location l = span(ls, right->loc.end);
        left = make_assignment(std::move(left), std::move(right), l);
        continue;
      }
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
    ExprBox rhs;
    if (cur().kind == Kind::DOT) {  // refutation  -> .
      Token d = cur(); advance();
      rhs = E({Pexp_unreachable{}, tokloc(d)});
    } else {
      rhs = parse_expr();
    }
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
        if (peek(1).kind == Kind::OPEN || peek(1).kind == Kind::MODULE) {
          // let open M in e  /  let module M = me in e  -> Pexp_struct_item
          advance();  // let
          StructureItem si = parse_structure_item();
          expect(Kind::IN, "in");
          ExprBox body = parse_expr();
          Location l = span(position(t.start), body->loc.end);
          return E({Pexp_struct_item{box(std::move(si)), std::move(body)}, l});
        }
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
        return E({Pexp_function{{}, std::nullopt, std::move(fb)}, span(position(t.start), last)});
      }
      case Kind::FUN: {
        advance();
        std::vector<FunctionParam> params;
        while (cur().kind != Kind::MINUSGREATER) params.push_back(parse_param());
        expect(Kind::MINUSGREATER, "->");
        ExprBox body = parse_expr();
        // `fun (type a) … -> e` with *only* newtype params is a Pexp_newtype chain.
        bool all_newtype = !params.empty();
        for (auto& p : params)
          if (!std::holds_alternative<Pparam_newtype>(p.desc)) all_newtype = false;
        if (all_newtype) {
          ExprBox acc = std::move(body);
          Position bend = acc->loc.end;
          for (int i = static_cast<int>(params.size()) - 1; i >= 0; --i) {
            auto& nt = std::get<Pparam_newtype>(params[i].desc);
            Position s = (i == 0) ? position(t.start) : nt.loc.start;  // outermost from `fun`
            acc = E({Pexp_newtype{nt.name, std::move(acc)}, Location{s, bend, i != 0}});
          }
          return acc;
        }
        Location l = span(position(t.start), body->loc.end);
        auto fb = box(FunctionBody{Pfunction_body{std::move(body)}});
        return E({Pexp_function{std::move(params), std::nullopt, std::move(fb)}, l});
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
      case Kind::LETOP: {  // let* p = e0 [and* …] in e
        Token op = cur(); advance();
        Pattern pat = parse_pattern();
        expect(Kind::EQUAL, "=");
        ExprBox e0 = parse_expr();
        BindingOp letb{StringLoc{op.text, tokloc(op)}, std::move(pat), std::move(e0),
                       span(position(op.start), position(tokens_[idx_ - 1].end))};
        std::vector<BindingOp> ands;
        while (cur().kind == Kind::ANDOP) {
          Token aop = cur(); advance();
          Pattern ap = parse_pattern();
          expect(Kind::EQUAL, "=");
          ExprBox ae = parse_expr();
          ands.push_back(BindingOp{StringLoc{aop.text, tokloc(aop)}, std::move(ap), std::move(ae),
                                   span(position(aop.start), position(tokens_[idx_ - 1].end))});
        }
        expect(Kind::IN, "in");
        ExprBox body = parse_expr();
        Location l = span(position(t.start), body->loc.end);
        return E({Pexp_letop{std::move(letb), std::move(ands), std::move(body)}, l});
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
  // Compound type nodes take their location from the *symbol* span (which
  // includes a parenthesized child's parens), not the child node's own loc.
  CoreTypeBox parse_type_arrow() {
    Position symstart = position(cur().start);
    CoreTypeBox t = parse_type_tuple();
    if (cur().kind == Kind::MINUSGREATER) {
      advance();
      CoreTypeBox cod = parse_type_arrow();
      Location l = span(symstart, position(tokens_[idx_ - 1].end));
      return box(CoreType{Ptyp_arrow{Nolabel{}, std::move(t), std::move(cod)}, l});
    }
    return t;
  }
  CoreTypeBox parse_type_tuple() {
    Position symstart = position(cur().start);
    CoreTypeBox t = parse_type_app();
    if (cur().kind != Kind::STAR) return t;
    std::vector<CoreTypeBox> elems;
    elems.push_back(std::move(t));
    while (cur().kind == Kind::STAR) { advance(); elems.push_back(parse_type_app()); }
    Location l = span(symstart, position(tokens_[idx_ - 1].end));
    return box(CoreType{Ptyp_tuple{std::move(elems)}, l});
  }
  CoreTypeBox parse_type_app() {
    Position symstart = position(cur().start);
    CoreTypeBox t = parse_type_atom();
    while (cur().kind == Kind::LIDENT || cur().kind == Kind::UIDENT) {
      LongidentLoc name = parse_longident_path();
      std::vector<CoreTypeBox> args;
      args.push_back(std::move(t));
      t = box(CoreType{.desc = Ptyp_constr{.id = name, .args = std::move(args)},
                       .loc = span(symstart, name.loc.end)});
    }
    while (cur().kind == Kind::LBRACKETAT) {  // t [@attr]  -> ptyp_attributes
      advance();
      t->attrs.push_back(parse_attribute_body());
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
    if (t.kind == Kind::LPAREN && peek(1).kind == Kind::MODULE) {  // (module S [with type …])
      advance(); advance();  // ( module
      LongidentLoc path = parse_longident_path();
      std::vector<std::pair<LongidentLoc, CoreTypeBox>> cons;
      if (cur().kind == Kind::WITH) {
        advance();
        for (;;) {
          expect(Kind::TYPE, "type");
          LongidentLoc lp = parse_longident_path();
          expect(Kind::EQUAL, "=");
          cons.emplace_back(lp, parse_core_type());
          if (cur().kind == Kind::AND) { advance(); continue; }
          break;
        }
      }
      Token c = cur(); expect(Kind::RPAREN, ")");
      return box(CoreType{Ptyp_package{std::move(path), std::move(cons)},
                          span(position(t.start), position(c.end))});
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
      expect(Kind::RPAREN, ")");
      return inner;  // core types keep the inner loc (no paren reloc, unlike exprs)
    }
    if (t.kind == Kind::LBRACKET || t.kind == Kind::LBRACKETGREATER ||
        t.kind == Kind::LBRACKETLESS) {  // polymorphic variant type
      advance();
      ClosedFlag closed = (t.kind == Kind::LBRACKETGREATER) ? ClosedFlag::Open : ClosedFlag::Closed;
      std::vector<RowField> rows;
      if (cur().kind == Kind::BAR) advance();
      rows.push_back(parse_row_field());
      while (cur().kind == Kind::BAR) { advance(); rows.push_back(parse_row_field()); }
      std::optional<std::vector<std::string>> labels;
      // `[< … ]` always carries a present-tags list (Some, possibly empty).
      if (t.kind == Kind::LBRACKETLESS) labels.emplace();
      if (cur().kind == Kind::GREATER) {  // [< … > `a `b]
        advance();
        std::vector<std::string> ls;
        while (cur().kind == Kind::BACKQUOTE) { advance(); ls.push_back(cur().text); advance(); }
        labels = std::move(ls);
      }
      Token c = cur(); expect(Kind::RBRACKET, "]");
      return box(CoreType{.desc = Ptyp_variant{std::move(rows), closed, std::move(labels)},
                          .loc = span(position(t.start), position(c.end))});
    }
    if (t.kind == Kind::LESS) {  // object type  < m : t; … [;] [..] >
      advance();
      std::vector<ObjectField> fields;
      ClosedFlag closed = ClosedFlag::Closed;
      while (cur().kind != Kind::GREATER) {
        if (cur().kind == Kind::DOTDOT) { advance(); closed = ClosedFlag::Open; break; }
        Token nm = cur();
        if (nm.kind == Kind::LIDENT && peek(1).kind == Kind::COLON) {
          advance(); advance();  // name :
          CoreTypeBox ty = parse_possibly_poly_type();
          fields.push_back(Otag{StringLoc{nm.text, tokloc(nm)}, std::move(ty)});
        } else {  // Oinherit: an inherited object type
          fields.push_back(Oinherit{parse_core_type()});
        }
        if (cur().kind == Kind::SEMI) advance(); else break;
      }
      Token c = cur(); expect(Kind::GREATER, ">");
      return box(CoreType{Ptyp_object{std::move(fields), closed},
                          span(position(t.start), position(c.end))});
    }
    throw ParseError("expected a type", t.start);
  }
  CoreTypeBox parse_possibly_poly_type() { return parse_core_type(); }
  RowField parse_row_field() {
    if (cur().kind == Kind::BACKQUOTE) {
      advance();
      Token tag = cur();
      advance();
      std::vector<CoreTypeBox> types;
      if (cur().kind == Kind::OF) {
        advance();
        if (cur().kind == Kind::AMPERSAND) advance();
        types.push_back(parse_core_type());
        while (cur().kind == Kind::AMPERSAND) { advance(); types.push_back(parse_core_type()); }
      }
      bool constant = types.empty();
      return Rtag{tag.text, constant, std::move(types)};
    }
    return Rinherit{parse_core_type()};
  }

  // ---- patterns ----
  static bool is_simple_pattern_start(Kind k) {
    switch (k) {
      case Kind::LIDENT: case Kind::UNDERSCORE: case Kind::LPAREN: case Kind::INT:
      case Kind::FLOAT: case Kind::CHAR: case Kind::STRING: case Kind::UIDENT:
      case Kind::TRUE: case Kind::FALSE: case Kind::LBRACKET: case Kind::LBRACE:
      case Kind::LBRACKETBAR: case Kind::LBRACKETPERCENT: case Kind::HASH:
      case Kind::BACKQUOTE: case Kind::LAZY:
        return true;
      default: return false;
    }
  }
  Pattern parse_pattern() {
    if (cur().kind == Kind::EFFECT) {  // effect P, k  (effect handler pattern)
      Token t = cur(); advance();
      Pattern eff = parse_pat_app();
      expect(Kind::COMMA, ",");
      Pattern cont = parse_simple_pattern();
      Location l = span(position(t.start), cont.loc.end);
      return Pattern{Ppat_effect{box(std::move(eff)), box(std::move(cont))}, l};
    }
    if (cur().kind == Kind::EXCEPTION) {
      Token t = cur(); advance();
      Pattern p = parse_pat_alias();
      Location l = span(position(t.start), p.loc.end);
      return Pattern{Ppat_exception{box(std::move(p))}, l};
    }
    return parse_pat_alias();
  }
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
    if (cur().kind == Kind::LAZY) {
      Token t = cur(); advance();
      Pattern arg = parse_simple_pattern();
      Location l = span(position(t.start), arg.loc.end);
      return Pattern{Ppat_lazy{box(std::move(arg))}, l};
    }
    if (cur().kind == Kind::BACKQUOTE) {
      Token t = cur(); advance();
      Token tag = cur(); advance();
      if (is_simple_pattern_start(cur().kind)) {
        Pattern arg = parse_simple_pattern();
        Location l = span(position(t.start), arg.loc.end);
        return Pattern{Ppat_variant{tag.text, box(std::move(arg))}, l};
      }
      return Pattern{Ppat_variant{tag.text, std::nullopt},
                     span(position(t.start), position(tag.end))};
    }
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
      case Kind::INT: case Kind::FLOAT: case Kind::CHAR: case Kind::STRING: {
        advance();
        Constant c1 = const_of(t);
        if (cur().kind == Kind::DOTDOT) {  // interval  c1 .. c2
          advance();
          Token t2 = cur();
          if (t2.kind != Kind::INT && t2.kind != Kind::FLOAT &&
              t2.kind != Kind::CHAR && t2.kind != Kind::STRING)
            throw ParseError("expected constant in interval", t2.start);
          advance();
          return {Ppat_interval{std::move(c1), const_of(t2)},
                  span(position(t.start), position(t2.end))};
        }
        return {Ppat_constant{std::move(c1)}, tokloc(t)};
      }
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
        if (cur().kind == Kind::MODULE) {  // (module M [: S])
          advance();
          StrOptLoc name;
          if (cur().kind == Kind::UIDENT) { Token nm = cur(); advance(); name = StrOptLoc{nm.text, tokloc(nm)}; }
          else if (cur().kind == Kind::UNDERSCORE) { Token nm = cur(); advance(); name = StrOptLoc{std::nullopt, tokloc(nm)}; }
          else throw ParseError("expected module name", cur().start);
          if (cur().kind == Kind::COLON) { advance(); parse_module_type(); }
          Token c = cur(); expect(Kind::RPAREN, ")");
          return {Ppat_unpack{std::move(name)}, span(position(t.start), position(c.end))};
        }
        if (peek(1).kind == Kind::RPAREN) {
          if (auto op = operator_name(cur())) {  // (+) x = …  -> Ppat_var "+"
            advance();
            Token c = cur(); advance();  // RPAREN
            Location l = span(position(t.start), position(c.end));
            return {Ppat_var{StringLoc{*op, l}}, l};
          }
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
      case Kind::LBRACKETBAR: {  // [| p; … |]
        advance();
        std::vector<PatBox> elems;
        if (cur().kind != Kind::BARRBRACKET) {
          elems.push_back(box(parse_pattern()));
          while (cur().kind == Kind::SEMI) {
            advance();
            if (cur().kind == Kind::BARRBRACKET) break;
            elems.push_back(box(parse_pattern()));
          }
        }
        Token c = cur(); expect(Kind::BARRBRACKET, "|]");
        return {Ppat_array{std::move(elems)}, span(position(t.start), position(c.end))};
      }
      case Kind::LBRACKETPERCENT: {  // [%id payload]
        advance();
        std::string name = parse_attr_name();
        Structure payload = parse_structure_until(Kind::RBRACKET);
        Token c = cur(); expect(Kind::RBRACKET, "]");
        return {Ppat_extension{std::move(name), std::move(payload)},
                span(position(t.start), position(c.end))};
      }
      case Kind::HASH: {  // #tconst
        advance();
        LongidentLoc id = parse_longident_path();
        return {Ppat_type{id}, span(position(t.start), id.loc.end)};
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
    std::optional<CoreTypeBox> res;
    Position endp = position(nm.end);
    if (cur().kind == Kind::COLON) {  // GADT:  A : t1 * … * tn -> tres   (or  A : tres)
      advance();
      std::vector<CoreTypeBox> ts;
      ts.push_back(parse_type_app());
      while (cur().kind == Kind::STAR) { advance(); ts.push_back(parse_type_app()); }
      if (cur().kind == Kind::MINUSGREATER) {
        advance();
        res = parse_core_type();
        args = Pcstr_tuple{std::move(ts)};
      } else {
        res = std::move(ts[0]);  // no arrow: the lone type is the result, no args
      }
      endp = position(tokens_[idx_ - 1].end);
    } else if (cur().kind == Kind::OF) {
      advance();
      if (cur().kind == Kind::LBRACE) {
        auto fs = parse_label_decls();
        endp = position(tokens_[idx_ - 1].end);
        args = Pcstr_record{std::move(fs)};
      } else {
        std::vector<CoreTypeBox> ts;
        ts.push_back(parse_type_app());
        while (cur().kind == Kind::STAR) { advance(); ts.push_back(parse_type_app()); }
        endp = position(tokens_[idx_ - 1].end);
        args = Pcstr_tuple{std::move(ts)};
      }
    }
    return ConstructorDecl{StringLoc{nm.text, tokloc(nm)}, std::move(args), std::move(res),
                           span(start, endp)};
  }
  CoreTypeBox parse_type_param() {
    if (cur().kind == Kind::PLUS || cur().kind == Kind::MINUS) advance();  // variance (dropped)
    if (cur().kind == Kind::UNDERSCORE) {
      Token u = cur(); advance();
      return box(CoreType{Ptyp_any{}, tokloc(u)});
    }
    return parse_type_atom();  // 'a
  }
  std::vector<CoreTypeBox> parse_type_params() {
    std::vector<CoreTypeBox> params;
    Kind k = cur().kind;
    if (k == Kind::QUOTE || k == Kind::UNDERSCORE || k == Kind::PLUS || k == Kind::MINUS) {
      params.push_back(parse_type_param());
    } else if (k == Kind::LPAREN) {
      advance();
      params.push_back(parse_type_param());
      while (cur().kind == Kind::COMMA) { advance(); params.push_back(parse_type_param()); }
      expect(Kind::RPAREN, ")");
    }
    return params;
  }
  ExtensionConstructor parse_ext_ctor(Position start) {
    Token nm = cur();
    if (nm.kind != Kind::UIDENT) throw ParseError("expected constructor name", nm.start);
    advance();
    std::variant<Pext_decl, Pext_rebind> kind;
    Position endp = position(nm.end);
    if (cur().kind == Kind::EQUAL) {  // rebind
      advance();
      LongidentLoc path = parse_longident_path();
      endp = path.loc.end;
      kind = Pext_rebind{path};
    } else {
      ConstructorArguments args = Pcstr_tuple{};
      std::optional<CoreTypeBox> res;
      if (cur().kind == Kind::COLON) {  // GADT-style
        advance();
        std::vector<CoreTypeBox> ts;
        ts.push_back(parse_type_app());
        while (cur().kind == Kind::STAR) { advance(); ts.push_back(parse_type_app()); }
        if (cur().kind == Kind::MINUSGREATER) { advance(); res = parse_core_type(); args = Pcstr_tuple{std::move(ts)}; }
        else res = std::move(ts[0]);
        endp = position(tokens_[idx_ - 1].end);
      } else if (cur().kind == Kind::OF) {
        advance();
        if (cur().kind == Kind::LBRACE) { args = Pcstr_record{parse_label_decls()}; endp = position(tokens_[idx_ - 1].end); }
        else {
          std::vector<CoreTypeBox> ts;
          ts.push_back(parse_type_app());
          while (cur().kind == Kind::STAR) { advance(); ts.push_back(parse_type_app()); }
          endp = position(tokens_[idx_ - 1].end);
          args = Pcstr_tuple{std::move(ts)};
        }
      }
      kind = Pext_decl{std::move(args), std::move(res)};
    }
    return ExtensionConstructor{StringLoc{nm.text, tokloc(nm)}, std::move(kind), span(start, endp)};
  }
  TypeKind parse_type_kind_body() {  // record / variant / open (cur at the kind start)
    if (cur().kind == Kind::DOTDOT) { advance(); return Ptype_open{}; }
    if (cur().kind == Kind::LBRACE) return Ptype_record{parse_label_decls()};
    Position cs = position(cur().start);  // constructor loc includes a leading '|'
    if (cur().kind == Kind::BAR) advance();
    std::vector<ConstructorDecl> ctors;
    ctors.push_back(parse_constructor_decl(cs));
    while (cur().kind == Kind::BAR) {
      Position bs = position(cur().start);
      advance();
      ctors.push_back(parse_constructor_decl(bs));
    }
    return Ptype_variant{std::move(ctors)};
  }
  TypeDeclaration parse_type_declaration(Position declStart) {
    std::vector<CoreTypeBox> params = parse_type_params();
    Token nm = cur();
    if (nm.kind != Kind::LIDENT) throw ParseError("expected type name", nm.start);
    advance();
    TypeKind kind = Ptype_abstract{};
    std::optional<CoreTypeBox> manifest;
    PrivateFlag priv = PrivateFlag::Public;
    if (cur().kind == Kind::EQUAL) {
      advance();
      if (cur().kind == Kind::PRIVATE) { advance(); priv = PrivateFlag::Private; }
      // A `kind` (record/variant/open) starts with `{`, `..`, `|`, or an unqualified
      // constructor (UIDENT not followed by `.`); otherwise it's a manifest type,
      // optionally followed by `= [private] kind` (variant/record redefinition).
      bool kind_start = cur().kind == Kind::DOTDOT || cur().kind == Kind::LBRACE ||
                        cur().kind == Kind::BAR ||
                        (cur().kind == Kind::UIDENT && peek(1).kind != Kind::DOT);
      if (kind_start) {
        kind = parse_type_kind_body();
      } else {
        manifest = parse_core_type();
        if (cur().kind == Kind::EQUAL) {  // type t = manifest = [private] kind
          advance();
          if (cur().kind == Kind::PRIVATE) { advance(); priv = PrivateFlag::Private; }
          kind = parse_type_kind_body();
        }
      }
    }
    std::vector<TypeConstraint> constraints;  // constraint t1 = t2 …
    while (cur().kind == Kind::CONSTRAINT) {
      advance();
      CoreTypeBox t1 = parse_core_type();
      expect(Kind::EQUAL, "=");
      CoreTypeBox t2 = parse_core_type();
      Location cl = span(t1->loc.start, position(tokens_[idx_ - 1].end));  // `t1 = t2` span
      constraints.push_back(TypeConstraint{std::move(t1), std::move(t2), cl});
    }
    Attributes attrs;
    while (cur().kind == Kind::LBRACKETATAT) { advance(); attrs.push_back(parse_attribute_body()); }
    Location l = span(declStart, position(tokens_[idx_ - 1].end));
    return TypeDeclaration{StringLoc{nm.text, tokloc(nm)}, std::move(params),
                           std::move(kind), priv, std::move(manifest), l, std::move(attrs),
                           std::move(constraints)};
  }

  // ---- bindings ----
  static bool is_param_start(Kind k) {
    return k == Kind::LABEL || k == Kind::OPTLABEL || k == Kind::TILDE ||
           k == Kind::QUESTION || is_simple_pattern_start(k);
  }
  FunctionParam parse_param() {
    Token t = cur();
    // (type a)  locally abstract type parameter
    if (t.kind == Kind::LPAREN && peek(1).kind == Kind::TYPE) {
      advance(); advance();  // ( type
      Token id = cur();
      if (id.kind != Kind::LIDENT) throw ParseError("expected type name", id.start);
      advance();
      Token c = cur(); expect(Kind::RPAREN, ")");
      return FunctionParam{Pparam_newtype{StringLoc{id.text, tokloc(id)},
                                          span(position(t.start), position(c.end))}};
    }
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
    // ~lbl:pat   (labelled with explicit pattern)
    if (t.kind == Kind::LABEL) {
      advance();
      Pattern p = parse_simple_pattern();
      Location loc = span(position(t.start), p.loc.end);
      return FunctionParam{Pparam_val{loc, Labelled{t.text}, std::nullopt, std::move(p)}};
    }
    // ?lbl:pat  or  ?lbl:(pat [: t] = e)   (optional with explicit pattern)
    if (t.kind == Kind::OPTLABEL) {
      advance();
      if (cur().kind == Kind::LPAREN) {
        advance();
        Pattern p = parse_pattern();
        if (cur().kind == Kind::COLON) {
          advance(); CoreTypeBox ty = parse_core_type();
          Location pl = p.loc;
          p = Pattern{Ppat_constraint{box(std::move(p)), std::move(ty)}, pl};
        }
        std::optional<ExprBox> def;
        if (cur().kind == Kind::EQUAL) { advance(); def = parse_expr(); }
        Token c = cur(); expect(Kind::RPAREN, ")");
        Location loc = span(position(t.start), position(c.end));
        return FunctionParam{Pparam_val{loc, Optional{t.text}, std::move(def), std::move(p)}};
      }
      Pattern p = parse_simple_pattern();
      Location loc = span(position(t.start), p.loc.end);
      return FunctionParam{Pparam_val{loc, Optional{t.text}, std::nullopt, std::move(p)}};
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
    ValueBinding vb = parse_value_binding_core();
    while (cur().kind == Kind::LBRACKETATAT) {  // trailing  [@@attr]
      advance();
      vb.attrs.push_back(parse_attribute_body());
    }
    return vb;
  }
  ValueBinding parse_value_binding_core() {
    // val_ident form (`let f p.. = e` / `let (+) p.. = e`) vs pattern form.
    bool op_ident = cur().kind == Kind::LPAREN && operator_name(peek(1)) &&
                    peek(2).kind == Kind::RPAREN;
    bool val_ident = op_ident ||
                     (cur().kind == Kind::LIDENT &&
                      (peek(1).kind == Kind::EQUAL || peek(1).kind == Kind::COLON ||
                       is_param_start(peek(1).kind)));
    if (!val_ident) {
      Pattern pat = parse_pattern();
      expect(Kind::EQUAL, "=");
      ExprBox body = parse_expr();
      return ValueBinding{std::move(pat), std::move(body), std::nullopt};
    }
    StringLoc name;
    if (op_ident) {
      Token lp = cur(); advance();
      auto op = operator_name(cur()); advance();
      Token c = cur(); advance();  // RPAREN
      name = StringLoc{*op, span(position(lp.start), position(c.end))};
    } else {
      Token nt = cur(); advance();
      name = StringLoc{nt.text, tokloc(nt)};
    }
    Location nl = name.loc;
    Pattern namepat{Ppat_var{name}, nl};

    std::vector<FunctionParam> params;
    while (cur().kind != Kind::EQUAL && cur().kind != Kind::COLON)
      params.push_back(parse_param());
    std::optional<ValueConstraint> vconstr;     // `let x : t = e`  (params empty)
    std::optional<FunctionConstraint> fconstr;  // `let f p.. : t = e`  (return constraint)
    if (cur().kind == Kind::COLON) {
      advance();
      if (cur().kind == Kind::TYPE) {  // : type a b. t  (locally abstract univars)
        advance();
        std::vector<StringLoc> univars;
        while (cur().kind == Kind::LIDENT) {
          Token id = cur(); advance();
          univars.push_back(StringLoc{id.text, tokloc(id)});
        }
        expect(Kind::DOT, ".");
        vconstr = Pvc_constraint{std::move(univars), parse_core_type()};
      } else {
        CoreTypeBox ty = parse_core_type();
        if (cur().kind == Kind::COLONGREATER) {  // : t :> t2
          advance();
          CoreTypeBox ty2 = parse_core_type();
          if (params.empty()) vconstr = Pvc_coercion{std::move(ty), std::move(ty2)};
          else fconstr = Pcoerce{std::move(ty), std::move(ty2)};
        } else if (params.empty()) {
          vconstr = Pvc_constraint{{}, std::move(ty)};
        } else {
          fconstr = Pconstraint{std::move(ty)};
        }
      }
    }
    expect(Kind::EQUAL, "=");
    ExprBox body = parse_expr();

    if (params.empty() && !fconstr)
      return ValueBinding{std::move(namepat), std::move(body), std::move(vconstr)};
    // desugar `let f p.. [: t] = e` to a ghost Pexp_function spanning p[0]..e
    auto param_loc = [](const FunctionParam& p) {
      if (auto* v = std::get_if<Pparam_val>(&p.desc)) return v->loc;
      return std::get<Pparam_newtype>(p.desc).loc;
    };
    Position fstart = params.empty() ? body->loc.start : param_loc(params.front()).start;
    Location floc = span(fstart, body->loc.end, /*ghost=*/true);
    auto fb = box(FunctionBody{Pfunction_body{std::move(body)}});
    ExprBox fn = E({Pexp_function{std::move(params), std::move(fconstr), std::move(fb)}, floc});
    return ValueBinding{std::move(namepat), std::move(fn), std::nullopt};
  }

  std::pair<RecFlag, std::vector<ValueBinding>> parse_value_bindings() {
    expect(Kind::LET, "let");
    Attributes letattrs;  // `let[@attr] …`  -> attached to the first binding
    while (cur().kind == Kind::LBRACKETAT) { advance(); letattrs.push_back(parse_attribute_body()); }
    RecFlag rf = RecFlag::Nonrecursive;
    if (cur().kind == Kind::REC) { advance(); rf = RecFlag::Recursive; }
    std::vector<ValueBinding> binds;
    binds.push_back(parse_value_binding());
    if (!letattrs.empty()) {  // prepend the let-attrs before any trailing [@@attr]
      for (auto& a : binds[0].attrs) letattrs.push_back(std::move(a));
      binds[0].attrs = std::move(letattrs);
    }
    while (cur().kind == Kind::AND) { advance(); binds.push_back(parse_value_binding()); }
    return {rf, std::move(binds)};
  }

  // ---- structure ----
  StructureItem parse_structure_item() {
    Token t = cur();
    if (t.kind == Kind::LET) {
      if (peek(1).kind == Kind::OPEN || peek(1).kind == Kind::MODULE) {
        // let open/module … in …  is an expression statement
        ExprBox e = parse_expr();
        Location l = e->loc;
        return StructureItem{Pstr_eval{std::move(e)}, l};
      }
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
    if (t.kind == Kind::INCLUDE) {
      advance();
      ModuleExpr me = parse_module_expr();
      Location l = span(position(t.start), position(tokens_[idx_ - 1].end));
      return StructureItem{Pstr_include{std::move(me)}, l};
    }
    if (t.kind == Kind::TYPE) {
      advance();
      RecFlag rf = RecFlag::Recursive;  // `type` is recursive by default
      if (cur().kind == Kind::NONREC) { advance(); rf = RecFlag::Nonrecursive; }
      Position d0 = position(t.start);
      // disambiguate `type [params] path += …` (extension) from declarations
      size_t save = idx_;
      std::vector<CoreTypeBox> params = parse_type_params();
      if (cur().kind == Kind::LIDENT || cur().kind == Kind::UIDENT) {
        LongidentLoc path = parse_longident_path();
        if (cur().kind == Kind::PLUSEQ) {
          advance();
          PrivateFlag priv = PrivateFlag::Public;
          if (cur().kind == Kind::PRIVATE) { advance(); priv = PrivateFlag::Private; }
          std::vector<ExtensionConstructor> ctors;
          Position cs = position(cur().start);  // first ctor: from leading '|' if present
          if (cur().kind == Kind::BAR) advance();
          ctors.push_back(parse_ext_ctor(cs));
          while (cur().kind == Kind::BAR) {
            Position bs = position(cur().start);
            advance();
            ctors.push_back(parse_ext_ctor(bs));
          }
          Location l = span(d0, position(tokens_[idx_ - 1].end));
          return StructureItem{
              Pstr_typext{TypeExtension{std::move(path), std::move(params), std::move(ctors), priv}}, l};
        }
      }
      idx_ = save;  // not an extension: parse type declaration(s)
      std::vector<TypeDeclaration> decls;
      decls.push_back(parse_type_declaration(d0));
      while (cur().kind == Kind::AND) {
        Position ds = position(cur().start);
        advance();
        decls.push_back(parse_type_declaration(ds));
      }
      Location l = span(d0, position(tokens_[idx_ - 1].end));
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
    if (t.kind == Kind::EXCEPTION) {
      advance();
      // extension_constructor loc spans the `exception` keyword
      ExtensionConstructor ctor = parse_ext_ctor(position(t.start));
      Location l = span(position(t.start), position(tokens_[idx_ - 1].end));
      return StructureItem{Pstr_exception{TypeException{std::move(ctor)}}, l};
    }
    if (t.kind == Kind::EXTERNAL) {
      advance();
      Token nm = cur();
      if (nm.kind != Kind::LIDENT) throw ParseError("expected external name", nm.start);
      advance();
      expect(Kind::COLON, ":");
      CoreTypeBox ty = parse_core_type();
      expect(Kind::EQUAL, "=");
      std::vector<std::string> prims;
      while (cur().kind == Kind::STRING) { prims.push_back(cur().text); advance(); }
      if (prims.empty()) throw ParseError("expected primitive string", cur().start);
      Attributes pattrs;
      while (cur().kind == Kind::LBRACKETATAT) { advance(); pattrs.push_back(parse_attribute_body()); }
      Location l = span(position(t.start), position(tokens_[idx_ - 1].end));
      PrimitiveDescription pd{StringLoc{nm.text, tokloc(nm)}, std::move(ty), std::move(prims), l,
                              std::move(pattrs)};
      return StructureItem{Pstr_primitive{std::move(pd)}, l};
    }
    if (t.kind == Kind::MODULE && peek(1).kind == Kind::TYPE) {
      advance(); advance();  // module type
      Token nm = cur();
      if (nm.kind != Kind::UIDENT) throw ParseError("expected module type name", nm.start);
      advance();
      std::optional<ModuleType> mty;
      if (cur().kind == Kind::EQUAL) { advance(); mty = parse_module_type(); }
      Location l = span(position(t.start), position(tokens_[idx_ - 1].end));
      return StructureItem{Pstr_modtype{StringLoc{nm.text, tokloc(nm)}, std::move(mty)}, l};
    }
    if (t.kind == Kind::MODULE && peek(1).kind == Kind::REC) {
      advance(); advance();  // module rec
      std::vector<ModuleBinding> binds;
      binds.push_back(parse_module_binding_def());
      while (cur().kind == Kind::AND) { advance(); binds.push_back(parse_module_binding_def()); }
      Location l = span(position(t.start), position(tokens_[idx_ - 1].end));
      return StructureItem{Pstr_recmodule{std::move(binds)}, l};
    }
    if (t.kind == Kind::MODULE) {
      advance();
      StrOptLoc name = parse_module_name();
      std::vector<std::pair<Position, FunctorParam>> params;
      while (cur().kind == Kind::LPAREN) {
        Position ps = position(cur().start);
        params.emplace_back(ps, parse_functor_param());
      }
      std::optional<ModuleType> cmty;
      Position colon_pos{};
      if (cur().kind == Kind::COLON) { colon_pos = position(cur().start); advance(); cmty = parse_module_type(); }
      expect(Kind::EQUAL, "=");
      ModuleExpr me = parse_module_expr();
      if (cmty) {
        Location cl = span(colon_pos, me.loc.end);
        me = ModuleExpr{Pmod_constraint{box(std::move(me)), box(std::move(*cmty))}, cl};
      }
      for (int i = static_cast<int>(params.size()) - 1; i >= 0; --i) {
        Location fl = span(params[i].first, me.loc.end);
        me = ModuleExpr{Pmod_functor{std::move(params[i].second), box(std::move(me))}, fl};
      }
      Location l = span(position(t.start), position(tokens_[idx_ - 1].end));
      return StructureItem{Pstr_module{ModuleBinding{std::move(name), std::move(me)}}, l};
    }
    if (t.kind == Kind::CLASS && peek(1).kind == Kind::TYPE) {
      Position kw = position(t.start);
      advance(); advance();  // class type
      std::vector<ClassTypeDeclaration> decls;
      decls.push_back(parse_one_class_type_decl(kw));
      while (cur().kind == Kind::AND) {
        Position akw = position(cur().start); advance();
        decls.push_back(parse_one_class_type_decl(akw));
      }
      Location l = span(position(t.start), position(tokens_[idx_ - 1].end));
      return StructureItem{Pstr_class_type{std::move(decls)}, l};
    }
    if (t.kind == Kind::CLASS) {
      Position kw = position(t.start);
      advance();
      std::vector<ClassDeclaration> decls;
      decls.push_back(parse_one_class_decl(kw));
      while (cur().kind == Kind::AND) {
        Position akw = position(cur().start); advance();
        decls.push_back(parse_one_class_decl(akw));
      }
      Location l = span(position(t.start), position(tokens_[idx_ - 1].end));
      return StructureItem{Pstr_class{std::move(decls)}, l};
    }
    if (t.kind == Kind::LBRACKETATATAT) {  // [@@@ name payload ]  (floating attribute)
      advance();
      std::string name = parse_attr_name();
      Structure payload = parse_structure_until(Kind::RBRACKET);
      Token c = cur(); expect(Kind::RBRACKET, "]");
      return StructureItem{Pstr_attribute{std::move(name), std::move(payload)},
                           span(position(t.start), position(c.end))};
    }
    if (t.kind == Kind::LBRACKETPERCENTPERCENT) {  // [%% name payload ]  (item extension)
      advance();
      std::string name = parse_attr_name();
      Structure payload = parse_structure_until(Kind::RBRACKET);
      Token c = cur(); expect(Kind::RBRACKET, "]");
      return StructureItem{Pstr_extension{std::move(name), std::move(payload)},
                           span(position(t.start), position(c.end))};
    }
    ExprBox e = parse_expr();
    Location l = e->loc;
    return StructureItem{Pstr_eval{std::move(e)}, l};
  }

  std::string parse_attr_name() {
    Token first = cur();
    if (first.kind != Kind::LIDENT && first.kind != Kind::UIDENT)
      throw ParseError("expected attribute name", first.start);
    advance();
    std::string name = first.text;
    while (cur().kind == Kind::DOT &&
           (peek(1).kind == Kind::LIDENT || peek(1).kind == Kind::UIDENT)) {
      advance();
      name += "." + cur().text;
      advance();
    }
    return name;
  }
  // parse the body of an attribute (the `[@`/`[@@` has already been consumed):
  //   name payload ]   with a structure payload (PStr).
  Attribute parse_attribute_body() {
    std::string name = parse_attr_name();
    Structure payload = parse_structure_until(Kind::RBRACKET);
    expect(Kind::RBRACKET, "]");
    return Attribute{std::move(name), std::move(payload)};
  }

  StrOptLoc parse_module_name() {
    Token nm = cur();
    if (nm.kind == Kind::UIDENT) { advance(); return StrOptLoc{nm.text, tokloc(nm)}; }
    if (nm.kind == Kind::UNDERSCORE) { advance(); return StrOptLoc{std::nullopt, tokloc(nm)}; }
    throw ParseError("expected module name", nm.start);
  }
  // ( X : S )  or  ( )
  FunctorParam parse_functor_param() {
    expect(Kind::LPAREN, "(");
    if (cur().kind == Kind::RPAREN) { advance(); return Functor_unit{}; }
    StrOptLoc name = parse_module_name();
    expect(Kind::COLON, ":");
    ModuleType mt = parse_module_type();
    expect(Kind::RPAREN, ")");
    return Functor_named{std::move(name), box(std::move(mt))};
  }

  Location none_loc() const { return Location{Position{0, 0, -1}, Position{0, 0, -1}, true}; }

  ModuleType parse_module_type() {
    ModuleType mt = parse_module_type_with();
    if (cur().kind == Kind::MINUSGREATER) {  // mt -> mt2  (anonymous functor sugar)
      advance();
      Position s = mt.loc.start;
      ModuleType cod = parse_module_type();
      FunctorParam param = Functor_named{StrOptLoc{std::nullopt, none_loc()}, box(std::move(mt))};
      Location l = span(s, cod.loc.end);
      return ModuleType{Pmty_functor{std::move(param), box(std::move(cod))}, l, {}};
    }
    return mt;
  }
  ModuleType parse_module_type_with() {
    ModuleType mt = parse_module_type_base();
    while (cur().kind == Kind::WITH) {
      advance();
      std::vector<WithConstraint> cs;
      cs.push_back(parse_with_constraint());
      while (cur().kind == Kind::AND) { advance(); cs.push_back(parse_with_constraint()); }
      Position s = mt.loc.start;
      Location l = span(s, position(tokens_[idx_ - 1].end));
      mt = ModuleType{Pmty_with{box(std::move(mt)), std::move(cs)}, l, {}};
    }
    return mt;
  }
  WithConstraint parse_with_constraint() {
    Token t = cur();
    if (t.kind == Kind::TYPE) {
      Position kw = position(t.start);
      advance();
      std::vector<CoreTypeBox> params = parse_type_params();
      LongidentLoc lid = parse_longident_path();
      bool subst = cur().kind == Kind::COLONEQUAL;
      if (subst) advance(); else expect(Kind::EQUAL, "=");
      PrivateFlag priv = PrivateFlag::Public;
      if (cur().kind == Kind::PRIVATE) { advance(); priv = PrivateFlag::Private; }
      CoreTypeBox manifest = parse_core_type();
      std::string lastnm = lid_last_name(lid.txt);
      Location nameloc = span(position(lid.loc.end.cnum - static_cast<int>(lastnm.size())), lid.loc.end);
      Location dl = span(kw, position(tokens_[idx_ - 1].end));
      auto td = box(TypeDeclaration{StringLoc{lastnm, nameloc}, std::move(params), TypeKind{Ptype_abstract{}},
                                    priv, std::move(manifest), dl, {}, {}});
      if (subst) return Pwith_typesubst{std::move(lid), std::move(td)};
      return Pwith_type{std::move(lid), std::move(td)};
    }
    if (t.kind == Kind::MODULE) {
      advance();
      LongidentLoc lid1 = parse_longident_path();
      bool subst = cur().kind == Kind::COLONEQUAL;
      if (subst) advance(); else expect(Kind::EQUAL, "=");
      LongidentLoc lid2 = parse_longident_path();
      if (subst) return Pwith_modsubst{std::move(lid1), std::move(lid2)};
      return Pwith_module{std::move(lid1), std::move(lid2)};
    }
    throw ParseError("unsupported with constraint", t.start);
  }
  ModuleType parse_module_type_base() {
    Token t = cur();
    if (t.kind == Kind::SIG) {
      advance();
      Signature items = parse_signature_until(Kind::END);
      Token c = cur(); expect(Kind::END, "end");
      return ModuleType{Pmty_signature{std::move(items)}, span(position(t.start), position(c.end)), {}};
    }
    if (t.kind == Kind::FUNCTOR) {
      advance();
      FunctorParam param = parse_functor_param();
      expect(Kind::MINUSGREATER, "->");
      ModuleType body = parse_module_type();
      return ModuleType{Pmty_functor{std::move(param), box(std::move(body))},
                        span(position(t.start), body.loc.end), {}};
    }
    if (t.kind == Kind::MODULE && peek(1).kind == Kind::TYPE && peek(2).kind == Kind::OF) {
      advance(); advance(); advance();  // module type of
      ModuleExpr me = parse_module_expr();
      return ModuleType{Pmty_typeof{box(std::move(me))}, span(position(t.start), me.loc.end), {}};
    }
    if (t.kind == Kind::LPAREN) {  // ( module_type )
      advance();
      ModuleType mt = parse_module_type();
      Token c = cur(); expect(Kind::RPAREN, ")");
      mt.loc = span(position(t.start), position(c.end));  // reloc to parens
      return mt;
    }
    if (t.kind == Kind::UIDENT) {
      LongidentLoc id = parse_longident_path();
      return ModuleType{Pmty_ident{id}, id.loc, {}};
    }
    throw ParseError("unsupported module type", t.start);
  }

  Signature parse_signature_until(Kind stop) {
    Signature items;
    while (cur().kind != Kind::TEOF && cur().kind != stop) {
      if (cur().kind == Kind::SEMISEMI) { advance(); continue; }
      items.push_back(parse_signature_item());
    }
    return items;
  }
  SignatureItem parse_signature_item() {
    Token t = cur();
    auto here = [&] { return span(position(t.start), position(tokens_[idx_ - 1].end)); };
    if (t.kind == Kind::VAL) {
      advance();
      Token nm = cur();
      if (nm.kind != Kind::LIDENT && nm.kind != Kind::LPAREN)
        throw ParseError("expected value name", nm.start);
      StringLoc vname;
      if (nm.kind == Kind::LPAREN && operator_name(peek(1)) && peek(2).kind == Kind::RPAREN) {
        advance(); auto op = operator_name(cur()); advance(); Token c = cur(); advance();
        vname = StringLoc{*op, span(position(nm.start), position(c.end))};
      } else { advance(); vname = StringLoc{nm.text, tokloc(nm)}; }
      expect(Kind::COLON, ":");
      CoreTypeBox ty = parse_core_type();
      Attributes attrs;
      while (cur().kind == Kind::LBRACKETATAT) { advance(); attrs.push_back(parse_attribute_body()); }
      Location l = here();
      return SignatureItem{Psig_value{ValueDescription{std::move(vname), std::move(ty), l, std::move(attrs)}}, l};
    }
    if (t.kind == Kind::EXTERNAL) {
      advance();
      Token nm = cur();
      if (nm.kind != Kind::LIDENT) throw ParseError("expected external name", nm.start);
      advance();
      expect(Kind::COLON, ":");
      CoreTypeBox ty = parse_core_type();
      expect(Kind::EQUAL, "=");
      std::vector<std::string> prims;
      while (cur().kind == Kind::STRING) { prims.push_back(cur().text); advance(); }
      if (prims.empty()) throw ParseError("expected primitive string", cur().start);
      Attributes attrs;
      while (cur().kind == Kind::LBRACKETATAT) { advance(); attrs.push_back(parse_attribute_body()); }
      Location l = here();
      return SignatureItem{Psig_primitive{PrimitiveDescription{
          StringLoc{nm.text, tokloc(nm)}, std::move(ty), std::move(prims), l, std::move(attrs)}}, l};
    }
    if (t.kind == Kind::TYPE) {
      advance();
      RecFlag rf = RecFlag::Recursive;
      if (cur().kind == Kind::NONREC) { advance(); rf = RecFlag::Nonrecursive; }
      Position d0 = position(t.start);
      size_t save = idx_;
      std::vector<CoreTypeBox> params = parse_type_params();
      if (cur().kind == Kind::LIDENT || cur().kind == Kind::UIDENT) {  // type [params] path += …
        LongidentLoc path = parse_longident_path();
        if (cur().kind == Kind::PLUSEQ) {
          advance();
          PrivateFlag priv = PrivateFlag::Public;
          if (cur().kind == Kind::PRIVATE) { advance(); priv = PrivateFlag::Private; }
          std::vector<ExtensionConstructor> ctors;
          Position cs = position(cur().start);
          if (cur().kind == Kind::BAR) advance();
          ctors.push_back(parse_ext_ctor(cs));
          while (cur().kind == Kind::BAR) {
            Position bs = position(cur().start); advance();
            ctors.push_back(parse_ext_ctor(bs));
          }
          Location l = here();
          return SignatureItem{Psig_typext{TypeExtension{std::move(path), std::move(params),
                                                         std::move(ctors), priv}}, l};
        }
      }
      idx_ = save;
      std::vector<TypeDeclaration> decls;
      decls.push_back(parse_type_declaration(d0));
      while (cur().kind == Kind::AND) {
        Position ds = position(cur().start); advance();
        decls.push_back(parse_type_declaration(ds));
      }
      return SignatureItem{Psig_type{rf, std::move(decls)}, span(d0, position(tokens_[idx_ - 1].end))};
    }
    if (t.kind == Kind::EXCEPTION) {
      advance();
      ExtensionConstructor ctor = parse_ext_ctor(position(t.start));
      Location l = here();
      return SignatureItem{Psig_exception{TypeException{std::move(ctor)}}, l};
    }
    if (t.kind == Kind::OPEN) {
      advance();
      OverrideFlag ovr = OverrideFlag::Fresh;
      if (cur().kind == Kind::BANG) { advance(); ovr = OverrideFlag::Override; }
      LongidentLoc id = parse_longident_path();
      return SignatureItem{Psig_open{ovr, std::move(id)}, here()};
    }
    if (t.kind == Kind::INCLUDE) {
      advance();
      ModuleType mt = parse_module_type();
      return SignatureItem{Psig_include{std::move(mt)}, here()};
    }
    if (t.kind == Kind::MODULE && peek(1).kind == Kind::TYPE) {
      advance(); advance();  // module type
      Token nm = cur();
      if (nm.kind != Kind::UIDENT) throw ParseError("expected module type name", nm.start);
      advance();
      std::optional<ModuleType> mty;
      if (cur().kind == Kind::EQUAL) { advance(); mty = parse_module_type(); }
      return SignatureItem{Psig_modtype{StringLoc{nm.text, tokloc(nm)}, std::move(mty)}, here()};
    }
    if (t.kind == Kind::MODULE) {
      advance();
      StrOptLoc name = parse_module_name();
      // module M (X:S) … : mty   (functor module declaration)
      std::vector<std::pair<Position, FunctorParam>> fparams;
      while (cur().kind == Kind::LPAREN) {
        Position ps = position(cur().start);
        fparams.emplace_back(ps, parse_functor_param());
      }
      expect(Kind::COLON, ":");
      ModuleType mt = parse_module_type();
      for (int i = static_cast<int>(fparams.size()) - 1; i >= 0; --i) {
        Location fl = span(fparams[i].first, mt.loc.end);
        mt = ModuleType{Pmty_functor{std::move(fparams[i].second), box(std::move(mt))}, fl, {}};
      }
      return SignatureItem{Psig_module{ModuleDeclaration{std::move(name), box(std::move(mt))}}, here()};
    }
    if (t.kind == Kind::CLASS && peek(1).kind == Kind::TYPE) {
      Position kw = position(t.start);
      advance(); advance();
      std::vector<ClassTypeDeclaration> decls;
      decls.push_back(parse_one_class_type_decl(kw));
      while (cur().kind == Kind::AND) {
        Position akw = position(cur().start); advance();
        decls.push_back(parse_one_class_type_decl(akw));
      }
      return SignatureItem{Psig_class_type{std::move(decls)}, here()};
    }
    if (t.kind == Kind::LBRACKETATATAT) {  // [@@@ …]
      advance();
      std::string name = parse_attr_name();
      Structure payload = parse_structure_until(Kind::RBRACKET);
      Token c = cur(); expect(Kind::RBRACKET, "]");
      return SignatureItem{Psig_attribute{std::move(name), std::move(payload)},
                           span(position(t.start), position(c.end))};
    }
    if (t.kind == Kind::LBRACKETPERCENTPERCENT) {  // [%% …]
      advance();
      std::string name = parse_attr_name();
      Structure payload = parse_structure_until(Kind::RBRACKET);
      Token c = cur(); expect(Kind::RBRACKET, "]");
      return SignatureItem{Psig_extension{std::move(name), std::move(payload)},
                           span(position(t.start), position(c.end))};
    }
    throw ParseError("unsupported signature item", t.start);
  }

  // name [params] [: S] = me   (a binding in `module M …` / `module rec …`)
  ModuleBinding parse_module_binding_def() {
    StrOptLoc name = parse_module_name();
    std::vector<std::pair<Position, FunctorParam>> params;
    while (cur().kind == Kind::LPAREN) {
      Position ps = position(cur().start);
      params.emplace_back(ps, parse_functor_param());
    }
    std::optional<ModuleType> cmty;
    Position colon_pos{};
    if (cur().kind == Kind::COLON) { colon_pos = position(cur().start); advance(); cmty = parse_module_type(); }
    expect(Kind::EQUAL, "=");
    ModuleExpr me = parse_module_expr();
    if (cmty) {
      Location cl = span(colon_pos, me.loc.end);
      me = ModuleExpr{Pmod_constraint{box(std::move(me)), box(std::move(*cmty))}, cl};
    }
    for (int i = static_cast<int>(params.size()) - 1; i >= 0; --i) {
      Location fl = span(params[i].first, me.loc.end);
      me = ModuleExpr{Pmod_functor{std::move(params[i].second), box(std::move(me))}, fl};
    }
    return ModuleBinding{std::move(name), std::move(me)};
  }

  ModuleExpr parse_module_expr() {
    Position symstart = position(cur().start);
    ModuleExpr me = parse_module_expr_head();
    while (cur().kind == Kind::LPAREN) {  // F(X) / F()  functor application
      advance();
      if (cur().kind == Kind::RPAREN) {  // F ()  generative application
        Token c = cur(); advance();
        me = ModuleExpr{Pmod_apply_unit{box(std::move(me))}, span(symstart, position(c.end))};
        continue;
      }
      ModuleExpr arg = parse_module_expr();
      Token c = cur(); expect(Kind::RPAREN, ")");
      me = ModuleExpr{Pmod_apply{box(std::move(me)), box(std::move(arg))},
                      span(symstart, position(c.end))};
    }
    return me;
  }
  ModuleExpr parse_module_expr_head() {
    Token t = cur();
    if (t.kind == Kind::STRUCT) {
      advance();
      Structure items = parse_structure_until(Kind::END);
      Token c = cur(); expect(Kind::END, "end");
      return ModuleExpr{Pmod_structure{std::move(items)}, span(position(t.start), position(c.end))};
    }
    if (t.kind == Kind::FUNCTOR) {
      advance();
      FunctorParam param = parse_functor_param();
      expect(Kind::MINUSGREATER, "->");
      ModuleExpr body = parse_module_expr();
      return ModuleExpr{Pmod_functor{std::move(param), box(std::move(body))},
                        span(position(t.start), body.loc.end)};
    }
    if (t.kind == Kind::LPAREN) {
      advance();
      if (cur().kind == Kind::VAL) {  // (val e [: pkg])  first-class module unpack
        advance();
        ExprBox e = parse_expr();
        if (cur().kind == Kind::COLON) {
          advance();
          LongidentLoc pkgpath = parse_longident_path();
          Location pkgloc = pkgpath.loc;
          auto pkg = box(CoreType{Ptyp_package{std::move(pkgpath), {}}, pkgloc});
          Location cl = span(e->loc.start, pkg->loc.end);
          e = E({Pexp_constraint{std::move(e), std::move(pkg)}, cl});
        }
        Token c = cur(); expect(Kind::RPAREN, ")");
        return ModuleExpr{Pmod_unpack{std::move(e)}, span(position(t.start), position(c.end))};
      }
      ModuleExpr me = parse_module_expr();
      if (cur().kind == Kind::COLON) {  // (me : mt)
        advance();
        ModuleType mt = parse_module_type();
        Token c = cur(); expect(Kind::RPAREN, ")");
        return ModuleExpr{Pmod_constraint{box(std::move(me)), box(std::move(mt))},
                          span(position(t.start), position(c.end))};
      }
      Token c = cur(); expect(Kind::RPAREN, ")");
      return me;  // grouping keeps inner loc
    }
    if (t.kind == Kind::UIDENT) {
      LongidentLoc mp = parse_longident_path();
      return ModuleExpr{.desc = Pmod_ident{.id = mp}, .loc = mp.loc};
    }
    throw ParseError("unsupported module expression", t.start);
  }

  // ---- class language ----
  void skip_item_attrs() { while (cur().kind == Kind::LBRACKETAT) { advance(); parse_attribute_body(); } }
  void skip_post_attrs() { while (cur().kind == Kind::LBRACKETATAT) { advance(); parse_attribute_body(); } }

  // formal/actual class params:  [ p1, p2, … ]   (brackets, comma-separated)
  std::vector<CoreTypeBox> parse_class_params() {
    std::vector<CoreTypeBox> params;
    if (cur().kind != Kind::LBRACKET) return params;
    advance();
    params.push_back(parse_class_type_param());
    while (cur().kind == Kind::COMMA) { advance(); params.push_back(parse_class_type_param()); }
    expect(Kind::RBRACKET, "]");
    return params;
  }
  CoreTypeBox parse_class_type_param() {  // [+|-|!] 'a
    while (cur().kind == Kind::PLUS || cur().kind == Kind::MINUS || cur().kind == Kind::BANG)
      advance();
    return parse_type_atom();
  }

  ClassStructure parse_class_structure_body() {  // self pattern + fields, up to END
    Pattern self;
    if (cur().kind == Kind::LPAREN) {
      Token lp = cur(); advance();
      self = parse_pattern();
      if (cur().kind == Kind::COLON) {
        advance();
        CoreTypeBox ty = parse_core_type();
        Token c = cur(); expect(Kind::RPAREN, ")");
        self = Pattern{Ppat_constraint{box(std::move(self)), std::move(ty)},
                       span(position(lp.start), position(c.end))};
      } else {
        Token c = cur(); expect(Kind::RPAREN, ")");
        self.loc = span(position(lp.start), position(c.end));  // reloc_pat
      }
    } else {
      Position p = position(tokens_[idx_ - 1].end);  // ghpat at empty-rule position
      self = Pattern{Ppat_any{}, Location{p, p, true}};
    }
    std::vector<ClassField> fields;
    while (cur().kind != Kind::END && cur().kind != Kind::TEOF)
      fields.push_back(parse_class_field());
    return ClassStructure{std::move(self), std::move(fields)};
  }

  ClassField parse_class_field() {
    Token t = cur();
    Position fs = position(t.start);
    if (t.kind == Kind::INHERIT) {
      advance();
      OverrideFlag ovr = OverrideFlag::Fresh;
      if (cur().kind == Kind::BANG) { advance(); ovr = OverrideFlag::Override; }
      skip_item_attrs();
      ClassExpr ce = parse_class_expr();
      std::optional<StringLoc> as_;
      if (cur().kind == Kind::AS) {
        advance();
        Token nm = cur(); expect(Kind::LIDENT, "identifier");
        as_ = StringLoc{nm.text, tokloc(nm)};
      }
      skip_post_attrs();
      return ClassField{Pcf_inherit{ovr, box(std::move(ce)), std::move(as_)},
                        span(fs, position(tokens_[idx_ - 1].end)), {}};
    }
    if (t.kind == Kind::VAL) {
      advance();
      Position vstart = position(cur().start);  // `value` rule $sloc starts after VAL
      OverrideFlag ovr = OverrideFlag::Fresh;
      if (cur().kind == Kind::BANG) { advance(); ovr = OverrideFlag::Override; }
      skip_item_attrs();
      MutableFlag mut = MutableFlag::Immutable;
      bool virt = false;
      for (;;) {
        if (cur().kind == Kind::MUTABLE) { advance(); mut = MutableFlag::Mutable; }
        else if (cur().kind == Kind::VIRTUAL) { advance(); virt = true; }
        else break;
      }
      Token nm = cur(); advance();
      StringLoc name{nm.text, tokloc(nm)};
      ClassFieldKind kind;
      if (virt) {
        expect(Kind::COLON, ":");
        kind = Cfk_virtual{parse_core_type()};
      } else if (cur().kind == Kind::COLON) {
        advance();
        CoreTypeBox ty = parse_core_type();
        expect(Kind::EQUAL, "=");
        ExprBox e = parse_expr();
        Location cl = span(vstart, e->loc.end);  // mkexp_constraint (mkexp) ~loc:$sloc
        kind = Cfk_concrete{ovr, E({Pexp_constraint{std::move(e), std::move(ty)}, cl})};
      } else {
        expect(Kind::EQUAL, "=");
        kind = Cfk_concrete{ovr, parse_expr()};
      }
      skip_post_attrs();
      return ClassField{Pcf_val{name, mut, std::move(kind)},
                        span(fs, position(tokens_[idx_ - 1].end)), {}};
    }
    if (t.kind == Kind::METHOD) {
      advance();
      OverrideFlag ovr = OverrideFlag::Fresh;
      if (cur().kind == Kind::BANG) { advance(); ovr = OverrideFlag::Override; }
      skip_item_attrs();
      PrivateFlag priv = PrivateFlag::Public;
      bool virt = false;
      for (;;) {
        if (cur().kind == Kind::PRIVATE) { advance(); priv = PrivateFlag::Private; }
        else if (cur().kind == Kind::VIRTUAL) { advance(); virt = true; }
        else break;
      }
      Token nm = cur(); advance();
      StringLoc name{nm.text, tokloc(nm)};
      ClassFieldKind kind;
      if (virt) {
        expect(Kind::COLON, ":");
        kind = Cfk_virtual{parse_possibly_poly_type()};
      } else if (cur().kind == Kind::COLON) {
        advance();
        Position ts = position(cur().start);  // $startpos of possibly_poly_type
        CoreTypeBox ty = parse_possibly_poly_type();
        expect(Kind::EQUAL, "=");
        ExprBox e = parse_expr();
        Location pl = span(ts, e->loc.end, /*ghost=*/true);  // ghexp Pexp_poly loc
        kind = Cfk_concrete{ovr, E({Pexp_poly{std::move(e), std::move(ty)}, pl})};
      } else {
        // method m params… = e   ->  Pexp_poly(<fun>, None), loc = body's loc
        std::vector<std::pair<Position, FunctionParam>> params;
        while (cur().kind != Kind::EQUAL && cur().kind != Kind::COLON) {
          Position ps = position(cur().start);
          params.emplace_back(ps, parse_param());
        }
        std::optional<CoreTypeBox> rett;
        if (cur().kind == Kind::COLON) { advance(); rett = parse_core_type(); }
        expect(Kind::EQUAL, "=");
        ExprBox body = parse_expr();
        if (rett) {
          Location cl = body->loc;
          body = E({Pexp_constraint{std::move(body), std::move(*rett)}, cl});
        }
        if (!params.empty()) {
          Position fstart = params.front().first;
          Location floc = span(fstart, body->loc.end, true);
          std::vector<FunctionParam> ps;
          for (auto& pr : params) ps.push_back(std::move(pr.second));
          auto fb = box(FunctionBody{Pfunction_body{std::move(body)}});
          body = E({Pexp_function{std::move(ps), std::nullopt, std::move(fb)}, floc});
        }
        Location pl = body->loc;
        pl.ghost = true;  // ghexp Pexp_poly is always ghost
        kind = Cfk_concrete{ovr, E({Pexp_poly{std::move(body), std::nullopt}, pl})};
      }
      skip_post_attrs();
      return ClassField{Pcf_method{name, priv, std::move(kind)},
                        span(fs, position(tokens_[idx_ - 1].end)), {}};
    }
    if (t.kind == Kind::CONSTRAINT) {
      advance();
      skip_item_attrs();
      CoreTypeBox t1 = parse_core_type();
      expect(Kind::EQUAL, "=");
      CoreTypeBox t2 = parse_core_type();
      skip_post_attrs();
      return ClassField{Pcf_constraint{std::move(t1), std::move(t2)},
                        span(fs, position(tokens_[idx_ - 1].end)), {}};
    }
    if (t.kind == Kind::INITIALIZER) {
      advance();
      skip_item_attrs();
      ExprBox e = parse_expr();
      skip_post_attrs();
      return ClassField{Pcf_initializer{std::move(e)},
                        span(fs, position(tokens_[idx_ - 1].end)), {}};
    }
    throw ParseError("unsupported class field", t.start);
  }

  ClassExpr parse_class_expr() {
    Token t = cur();
    if (t.kind == Kind::FUN) {
      advance();
      return parse_class_fun_def();  // wrap_class_attrs keeps body loc (spans from 1st param)
    }
    if (t.kind == Kind::LET) {
      auto [rf, binds] = parse_value_bindings();
      expect(Kind::IN, "in");
      ClassExpr body = parse_class_expr();
      return ClassExpr{Pcl_let{rf, std::move(binds), box(std::move(body))},
                       span(position(t.start), body.loc.end), {}};
    }
    ClassExpr ce = parse_class_simple_expr();
    std::vector<std::pair<ArgLabel, ExprBox>> args;
    for (;;) {
      Kind k = cur().kind;
      if (k == Kind::LABEL) { Token lt = cur(); advance(); args.emplace_back(Labelled{lt.text}, parse_atom_postfix()); }
      else if (k == Kind::OPTLABEL) { Token lt = cur(); advance(); args.emplace_back(Optional{lt.text}, parse_atom_postfix()); }
      else if (k == Kind::TILDE && peek(1).kind == Kind::LIDENT) { advance(); Token id = cur(); advance(); args.emplace_back(Labelled{id.text}, ident_expr(id.text, tokloc(id))); }
      else if (k == Kind::QUESTION && peek(1).kind == Kind::LIDENT) { advance(); Token id = cur(); advance(); args.emplace_back(Optional{id.text}, ident_expr(id.text, tokloc(id))); }
      else if (is_atom_start(k)) args.emplace_back(Nolabel{}, parse_atom_postfix());
      else break;
    }
    if (!args.empty()) {
      Location l = span(ce.loc.start, args.back().second->loc.end);
      ce = ClassExpr{Pcl_apply{box(std::move(ce)), std::move(args)}, l, {}};
    }
    return ce;
  }

  ClassExpr parse_class_fun_def() {  // simple_param… -> class_expr
    Position ps = position(cur().start);
    FunctionParam fp = parse_param();
    auto& pv = std::get<Pparam_val>(fp.desc);
    ClassExpr body;
    if (cur().kind == Kind::MINUSGREATER) { advance(); body = parse_class_expr(); }
    else body = parse_class_fun_def();
    Location l = span(ps, body.loc.end);
    return ClassExpr{Pcl_fun{pv.label, std::move(pv.default_), std::move(pv.pat), box(std::move(body))}, l, {}};
  }

  ClassExpr parse_class_simple_expr() {
    Token t = cur();
    if (t.kind == Kind::OBJECT) {
      advance();
      ClassStructure cs = parse_class_structure_body();
      Token c = cur(); expect(Kind::END, "end");
      return ClassExpr{Pcl_structure{std::move(cs)}, span(position(t.start), position(c.end)), {}};
    }
    if (t.kind == Kind::LPAREN) {
      advance();
      ClassExpr ce = parse_class_expr();
      if (cur().kind == Kind::COLON) {
        advance();
        ClassType ct = parse_class_type();
        Token c = cur(); expect(Kind::RPAREN, ")");
        return ClassExpr{Pcl_constraint{box(std::move(ce)), box(std::move(ct))},
                         span(position(t.start), position(c.end)), {}};
      }
      Token c = cur(); expect(Kind::RPAREN, ")");
      return ce;  // parenthesized class expr keeps inner loc
    }
    // actual_class_parameters class_longident -> Pcl_constr
    std::vector<CoreTypeBox> tys;
    Position cs0 = position(t.start);
    bool bracketed = (t.kind == Kind::LBRACKET);
    if (bracketed) {
      advance();
      tys.push_back(parse_core_type());
      while (cur().kind == Kind::COMMA) { advance(); tys.push_back(parse_core_type()); }
      expect(Kind::RBRACKET, "]");
    }
    LongidentLoc id = parse_longident_path();
    Location l = bracketed ? span(cs0, id.loc.end) : id.loc;
    return ClassExpr{Pcl_constr{id, std::move(tys)}, l, {}};
  }

  ClassType parse_class_type() {
    Token t = cur();
    if (t.kind == Kind::LABEL || t.kind == Kind::OPTLABEL) {  // labelled arrow domain
      ArgLabel label = t.kind == Kind::LABEL ? ArgLabel{Labelled{t.text}} : ArgLabel{Optional{t.text}};
      advance();
      CoreTypeBox dom = parse_type_tuple();
      expect(Kind::MINUSGREATER, "->");
      ClassType cod = parse_class_type();
      return ClassType{Pcty_arrow{label, std::move(dom), box(std::move(cod))},
                       span(position(t.start), cod.loc.end), {}};
    }
    if (t.kind == Kind::OBJECT) {
      advance();
      ClassSignature cs = parse_class_sig_body();
      Token c = cur(); expect(Kind::END, "end");
      return ClassType{Pcty_signature{std::move(cs)}, span(position(t.start), position(c.end)), {}};
    }
    if (t.kind == Kind::LBRACKET) {  // [tys] clty_longident
      size_t save = idx_;
      try {
        advance();
        std::vector<CoreTypeBox> tys;
        tys.push_back(parse_core_type());
        while (cur().kind == Kind::COMMA) { advance(); tys.push_back(parse_core_type()); }
        expect(Kind::RBRACKET, "]");
        LongidentLoc id = parse_longident_path();
        return ClassType{Pcty_constr{id, std::move(tys)}, span(position(t.start), id.loc.end), {}};
      } catch (const ParseError&) { idx_ = save; }
    }
    CoreTypeBox dom = parse_type_tuple();
    if (cur().kind == Kind::MINUSGREATER) {
      advance();
      ClassType cod = parse_class_type();
      return ClassType{Pcty_arrow{Nolabel{}, std::move(dom), box(std::move(cod))},
                       span(position(t.start), cod.loc.end), {}};
    }
    if (auto* tc = std::get_if<Ptyp_constr>(&dom->desc))
      return ClassType{Pcty_constr{tc->id, std::move(tc->args)}, dom->loc, {}};
    throw ParseError("expected class type", t.start);
  }

  ClassSignature parse_class_sig_body() {  // self type + fields, up to END
    CoreTypeBox self;
    if (cur().kind == Kind::LPAREN) {
      advance();
      self = parse_core_type();
      expect(Kind::RPAREN, ")");
    } else {
      Position p = position(tokens_[idx_ - 1].end);
      self = box(CoreType{Ptyp_any{}, Location{p, p, true}});
    }
    std::vector<ClassTypeField> fields;
    while (cur().kind != Kind::END && cur().kind != Kind::TEOF)
      fields.push_back(parse_class_sig_field());
    return ClassSignature{std::move(self), std::move(fields)};
  }

  ClassTypeField parse_class_sig_field() {
    Token t = cur();
    Position fs = position(t.start);
    if (t.kind == Kind::INHERIT) {
      advance();
      skip_item_attrs();
      ClassType ct = parse_class_type();
      skip_post_attrs();
      return ClassTypeField{Pctf_inherit{box(std::move(ct))},
                            span(fs, position(tokens_[idx_ - 1].end)), {}};
    }
    if (t.kind == Kind::VAL) {
      advance();
      skip_item_attrs();
      MutableFlag mut = MutableFlag::Immutable;
      VirtualFlag virt = VirtualFlag::Concrete;
      for (;;) {
        if (cur().kind == Kind::MUTABLE) { advance(); mut = MutableFlag::Mutable; }
        else if (cur().kind == Kind::VIRTUAL) { advance(); virt = VirtualFlag::Virtual; }
        else break;
      }
      Token nm = cur(); advance();
      StringLoc name{nm.text, tokloc(nm)};
      expect(Kind::COLON, ":");
      CoreTypeBox ty = parse_core_type();
      skip_post_attrs();
      return ClassTypeField{Pctf_val{name, mut, virt, std::move(ty)},
                            span(fs, position(tokens_[idx_ - 1].end)), {}};
    }
    if (t.kind == Kind::METHOD) {
      advance();
      skip_item_attrs();
      PrivateFlag priv = PrivateFlag::Public;
      VirtualFlag virt = VirtualFlag::Concrete;
      for (;;) {
        if (cur().kind == Kind::PRIVATE) { advance(); priv = PrivateFlag::Private; }
        else if (cur().kind == Kind::VIRTUAL) { advance(); virt = VirtualFlag::Virtual; }
        else break;
      }
      Token nm = cur(); advance();
      StringLoc name{nm.text, tokloc(nm)};
      expect(Kind::COLON, ":");
      CoreTypeBox ty = parse_possibly_poly_type();
      skip_post_attrs();
      return ClassTypeField{Pctf_method{name, priv, virt, std::move(ty)},
                            span(fs, position(tokens_[idx_ - 1].end)), {}};
    }
    if (t.kind == Kind::CONSTRAINT) {
      advance();
      skip_item_attrs();
      CoreTypeBox t1 = parse_core_type();
      expect(Kind::EQUAL, "=");
      CoreTypeBox t2 = parse_core_type();
      skip_post_attrs();
      return ClassTypeField{Pctf_constraint{std::move(t1), std::move(t2)},
                            span(fs, position(tokens_[idx_ - 1].end)), {}};
    }
    throw ParseError("unsupported class sig field", t.start);
  }

  // after `class` (or `and`) consumed; kw = the keyword's start position
  ClassDeclaration parse_one_class_decl(Position kw) {
    skip_item_attrs();
    VirtualFlag virt = VirtualFlag::Concrete;
    if (cur().kind == Kind::VIRTUAL) { advance(); virt = VirtualFlag::Virtual; }
    std::vector<CoreTypeBox> params = parse_class_params();
    Token nm = cur();
    if (nm.kind != Kind::LIDENT) throw ParseError("expected class name", nm.start);
    advance();
    StringLoc name{nm.text, tokloc(nm)};
    ClassExpr body = parse_class_fun_binding();
    skip_post_attrs();
    return ClassDeclaration{virt, std::move(params), std::move(name), std::move(body),
                            span(kw, position(tokens_[idx_ - 1].end)), {}};
  }
  ClassExpr parse_class_fun_binding() {  // = ce | : ct = ce | param fun_binding
    Token t = cur();
    if (t.kind == Kind::EQUAL) { advance(); return parse_class_expr(); }
    if (t.kind == Kind::COLON) {
      advance();
      ClassType ct = parse_class_type();
      expect(Kind::EQUAL, "=");
      ClassExpr ce = parse_class_expr();
      return ClassExpr{Pcl_constraint{box(std::move(ce)), box(std::move(ct))},
                       span(position(t.start), ce.loc.end), {}};
    }
    Position ps = position(t.start);
    FunctionParam fp = parse_param();
    auto& pv = std::get<Pparam_val>(fp.desc);
    ClassExpr body = parse_class_fun_binding();
    return ClassExpr{Pcl_fun{pv.label, std::move(pv.default_), std::move(pv.pat), box(std::move(body))},
                     span(ps, body.loc.end), {}};
  }
  ClassTypeDeclaration parse_one_class_type_decl(Position kw) {
    skip_item_attrs();
    VirtualFlag virt = VirtualFlag::Concrete;
    if (cur().kind == Kind::VIRTUAL) { advance(); virt = VirtualFlag::Virtual; }
    std::vector<CoreTypeBox> params = parse_class_params();
    Token nm = cur();
    if (nm.kind != Kind::LIDENT) throw ParseError("expected class type name", nm.start);
    advance();
    StringLoc name{nm.text, tokloc(nm)};
    expect(Kind::EQUAL, "=");
    ClassType body = parse_class_type();
    skip_post_attrs();
    return ClassTypeDeclaration{virt, std::move(params), std::move(name), std::move(body),
                                span(kw, position(tokens_[idx_ - 1].end)), {}};
  }
};

}  // namespace

Structure parse_structure(std::string_view src) { return Parser(src).parse_structure(); }

}  // namespace cppcaml
