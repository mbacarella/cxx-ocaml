// Port of middle_end/flambda/inlining_stats.ml and the printers of
// inlining_stats_types.ml (see inlining_stats.hpp).
#include "cppcaml/typing/inlining_stats.hpp"

#include <fstream>
#include <map>
#include <memory>
#include <optional>

#include "cppcaml/typing/clflags.hpp"
#include "cppcaml/typing/misc.hpp"

namespace cppcaml::typing::inlining_stats {

using format::Formatter;
using format::fprintf;
namespace W = inlining_cost::whether_sufficient_benefit;

// node = Closure of Closure_id.t * Debuginfo.t | Call of ... | Inlined |
// Specialised of Closure_id.Set.t  (the list's cells, in the permanent
// zone: an environment's stack outlives the pass zones)
struct ClosureStackNode {
  enum class Kind : unsigned char { Closure, Call, Inlined, Specialised } kind;
  variable::t closure_id;
  debuginfo::t dbg;
  ClosureStack next;
};

namespace {
ClosureStack cons(ClosureStackNode::Kind k, variable::t id, const debuginfo::t& dbg, ClosureStack t) {
  return permanent_zone().make<ClosureStackNode>(ClosureStackNode{k, id, dbg, t});
}
bool is_call(ClosureStack t) { return t && t->kind == ClosureStackNode::Kind::Call; }
}  // namespace

namespace closure_stack {
ClosureStack note_entering_closure(ClosureStack t, variable::t closure_id, const debuginfo::t& dbg) {
  if (!clflags::inlining_report) return t;
  if (is_call(t)) misc::fatal_error("note_entering_closure: unexpected Call node");
  return cons(ClosureStackNode::Kind::Closure, closure_id, dbg, t);
}
// CR-someday lwhite: since calls do not have a unique id it is possible
// some calls will end up sharing nodes.
ClosureStack note_entering_call(ClosureStack t, variable::t closure_id, const debuginfo::t& dbg) {
  if (!clflags::inlining_report) return t;
  if (is_call(t)) misc::fatal_error("note_entering_call: unexpected Call node");
  return cons(ClosureStackNode::Kind::Call, closure_id, dbg, t);
}
ClosureStack note_entering_inlined(ClosureStack t) {
  if (!clflags::inlining_report) return t;
  if (!is_call(t)) misc::fatal_error("note_entering_inlined: missing Call node");
  return cons(ClosureStackNode::Kind::Inlined, nullptr, {}, t);
}
ClosureStack note_entering_specialised(ClosureStack t, const variable::Set&) {
  if (!clflags::inlining_report) return t;
  if (!is_call(t)) misc::fatal_error("note_entering_specialised: missing Call node");
  return cons(ClosureStackNode::Kind::Specialised, nullptr, {}, t);
}
}  // namespace closure_stack

namespace {
// log: (closure_stack, decision) list, the newest first
std::vector<std::pair<ClosureStack, Decision>>& log() {
  static std::vector<std::pair<ClosureStack, Decision>> l;  // (in order of recording)
  return l;
}

// ---- Inlining_stats_types' printers ---------------------------------------
void text(Formatter& ppf, std::string_view s) { inlining_cost::pp_print_text(ppf, s); }

void print_stars(Formatter& ppf, long n) { fprintf(ppf, "%s", std::string(static_cast<std::size_t>(n), '*')); }

void print_calculation(Formatter& ppf, long depth, std::string_view title, bool subfunctions, const Wsb& wsb) {
  ppf.open_vbox(depth + 2);
  fprintf(ppf, "@[<h>%a %s@]@;@;@[%a@]", [&](Formatter& f) { print_stars(f, depth + 1); }, std::string(title),
          [&](Formatter& f) { W::print_description(f, subfunctions, wsb); });
  ppf.close_box();
  ppf.print_newline();
  ppf.print_newline();
}

void summary(Formatter& ppf, const Inlined& i) {
  using K = Inlined::Kind;
  switch (i.kind) {
    case K::Classic_mode:
      text(ppf, "This function was inlined because it was small enough to be inlined in `-Oclassic'");
      break;
    case K::Annotation: text(ppf, "This function was inlined because of an annotation."); break;
    case K::Decl_local_to_application:
      text(ppf, "This function was inlined because it was local to this application.");
      break;
    case K::Without_subfunctions: case K::With_subfunctions:
      text(ppf, "This function was inlined because the expected benefit outweighed the change in code size.");
      break;
  }
}
void calculation(Formatter& ppf, long depth, const Inlined& i) {
  using K = Inlined::Kind;
  if (i.kind == K::Without_subfunctions) print_calculation(ppf, depth, "Inlining benefit calculation", false, i.wsb1);
  else if (i.kind == K::With_subfunctions) print_calculation(ppf, depth, "Inlining benefit calculation", true, i.wsb2);
}

void summary(Formatter& ppf, const NotInlined& i) {
  using K = NotInlined::Kind;
  switch (i.kind) {
    case K::Classic_mode:
      text(ppf, "This function was not inlined because it was too large to be inlined in `-Oclassic'.");
      break;
    case K::Above_threshold:
      text(ppf, "This function was not inlined because it was larger than the current size threshold");
      fprintf(ppf, "(%i)", i.size);
      break;
    case K::Annotation: text(ppf, "This function was not inlined because of an annotation."); break;
    case K::No_useful_approximations:
      text(ppf,
           "This function was not inlined because there was no useful information about any of its parameters, "
           "and it was not particularly small.");
      break;
    case K::Unrolling_depth_exceeded:
      text(ppf, "This function was not inlined because its unrolling depth was exceeded.");
      break;
    case K::Self_call: text(ppf, "This function was not inlined because it was a self call."); break;
    case K::Without_subfunctions: case K::With_subfunctions:
      text(ppf, "This function was not inlined because the expected benefit did not outweigh the change in code size.");
      break;
  }
}
void calculation(Formatter& ppf, long depth, const NotInlined& i) {
  using K = NotInlined::Kind;
  if (i.kind == K::Without_subfunctions) print_calculation(ppf, depth, "Inlining benefit calculation", false, i.wsb1);
  else if (i.kind == K::With_subfunctions) print_calculation(ppf, depth, "Inlining benefit calculation", true, i.wsb2);
}

void summary(Formatter& ppf, const Specialised& s) {
  using K = Specialised::Kind;
  if (s.kind == K::Annotation) text(ppf, "This function was specialised because of an annotation.");
  else
    text(ppf, "This function was specialised because the expected benefit outweighed the change in code size.");
}
void calculation(Formatter& ppf, long depth, const Specialised& s) {
  using K = Specialised::Kind;
  if (s.kind == K::Without_subfunctions) print_calculation(ppf, depth, "Specialising benefit calculation", false, s.wsb1);
  else if (s.kind == K::With_subfunctions) print_calculation(ppf, depth, "Specialising benefit calculation", true, s.wsb2);
}

void summary(Formatter& ppf, const NotSpecialised& s) {
  using K = NotSpecialised::Kind;
  switch (s.kind) {
    case K::Classic_mode:
      text(ppf, "This function was not specialised because it was compiled with `-Oclassic'.");
      break;
    case K::Above_threshold:
      text(ppf, "This function was not specialised because it was larger than the current size threshold");
      fprintf(ppf, "(%i)", s.size);
      break;
    case K::Annotation: text(ppf, "This function was not specialised because of an annotation."); break;
    case K::Not_recursive: text(ppf, "This function was not specialised because it is not recursive."); break;
    case K::Not_closed: text(ppf, "This function was not specialised because it is not closed."); break;
    case K::No_invariant_parameters:
      text(ppf, "This function was not specialised because it has no invariant parameters.");
      break;
    case K::No_useful_approximations:
      text(ppf,
           "This function was not specialised because there was no useful information about any of its invariant "
           "parameters.");
      break;
    case K::Self_call: text(ppf, "This function was not specialised because it was a self call."); break;
    case K::Not_beneficial:
      text(ppf,
           "This function was not specialised because the expected benefit did not outweigh the change in code "
           "size.");
      break;
  }
}
void calculation(Formatter& ppf, long depth, const NotSpecialised& s) {
  if (s.kind == NotSpecialised::Kind::Not_beneficial)
    print_calculation(ppf, depth, "Specialising benefit calculation", true, s.wsb2);
}

void summary(Formatter& ppf, Prevented p) {
  if (p == Prevented::Function_prevented_from_inlining)
    text(ppf, "This function was prevented from inlining or specialising.");
  else
    text(ppf, "This function was prevented from inlining or specialising because the inlining depth was exceeded.");
}

void summary(Formatter& ppf, const Decision& d) {
  using K = Decision::Kind;
  switch (d.kind) {
    case K::Prevented: summary(ppf, d.prevented); break;
    case K::Specialised: summary(ppf, d.specialised); break;
    case K::Inlined:
      fprintf(ppf, "@[<v>@[%a@]@;@;@[%a@]@]", [&](Formatter& f) { summary(f, d.not_specialised); },
              [&](Formatter& f) { summary(f, d.inlined); });
      break;
    case K::Unchanged:
      fprintf(ppf, "@[<v>@[%a@]@;@;@[%a@]@]", [&](Formatter& f) { summary(f, d.not_specialised); },
              [&](Formatter& f) { summary(f, d.not_inlined); });
      break;
  }
}
void calculation(Formatter& ppf, long depth, const Decision& d) {
  using K = Decision::Kind;
  switch (d.kind) {
    case K::Prevented: break;
    case K::Specialised: calculation(ppf, depth, d.specialised); break;
    case K::Inlined:
      calculation(ppf, depth, d.not_specialised);
      calculation(ppf, depth, d.inlined);
      break;
    case K::Unchanged:
      calculation(ppf, depth, d.not_specialised);
      calculation(ppf, depth, d.not_inlined);
      break;
  }
}

// ---- Inlining_report ----------------------------------------------------------
// Place.t = Debuginfo.t * Closure_id.t * kind
struct Place {
  debuginfo::t dbg;
  variable::t cl;
  bool call;  // kind: Call (else Closure)
};
struct PlaceLess {
  bool operator()(const Place& a, const Place& b) const {
    int c = debuginfo::compare(a.dbg, b.dbg);
    if (c != 0) return c < 0;
    c = variable::compare(a.cl, b.cl);
    if (c != 0) return c < 0;
    // Closure, Call -> 1; Call, Closure -> -1
    if (a.call == b.call) return false;
    return a.call;
  }
};
struct Node;
using Report = std::map<Place, std::unique_ptr<Node>, PlaceLess>;
struct Call {
  std::optional<Decision> decision;
  std::unique_ptr<Report> inlined;
  std::unique_ptr<Report> specialised;
};
struct Node {
  bool is_call;
  Report closure;  // Closure of t
  Call call;       // Call of call
};

// Prevented or unchanged decisions may be overridden by a later look at
// the same call.  Other decisions may also be "overridden" because calls
// are not uniquely identified.
void add_call_decision(Call& call, const Decision& decision) {
  using K = Decision::Kind;
  if (!call.decision) {
    call.decision = decision;
    return;
  }
  K old = call.decision->kind;
  if (decision.kind == K::Prevented) return;
  if (old == K::Prevented) call.decision = decision;
  else if (old == K::Specialised) return;
  else if (decision.kind == K::Specialised) call.decision = decision;
  else if (old == K::Inlined) return;
  else if (decision.kind == K::Inlined) call.decision = decision;
  // Unchanged, Unchanged: the call is kept
}

void add_decision(Report& t, const std::vector<ClosureStack>& stack, std::size_t k, const Decision& decision) {
  using SK = ClosureStackNode::Kind;
  if (k >= stack.size()) misc::fatal_error("Inlining_report.add_decision");  // (assert false)
  ClosureStack n = stack[k];
  if (n->kind == SK::Closure) {
    std::unique_ptr<Node>& v = t[Place{n->dbg, n->closure_id, false}];
    if (!v) v = std::make_unique<Node>(Node{false, {}, {}});
    else if (v->is_call) misc::fatal_error("Inlining_report.add_decision");  // (assert false)
    add_decision(v->closure, stack, k + 1, decision);
    return;
  }
  if (n->kind != SK::Call) misc::fatal_error("Inlining_report.add_decision");  // (assert false)
  std::unique_ptr<Node>& v = t[Place{n->dbg, n->closure_id, true}];
  if (!v) v = std::make_unique<Node>(Node{true, {}, {}});
  else if (!v->is_call) misc::fatal_error("Inlining_report.add_decision");  // (assert false)
  if (k + 1 == stack.size()) {
    add_call_decision(v->call, decision);
    return;
  }
  ClosureStack next = stack[k + 1];
  if (next->kind == SK::Inlined) {
    if (!v->call.inlined) v->call.inlined = std::make_unique<Report>();
    add_decision(*v->call.inlined, stack, k + 2, decision);
  } else if (next->kind == SK::Specialised) {
    if (!v->call.specialised) v->call.specialised = std::make_unique<Report>();
    add_decision(*v->call.specialised, stack, k + 2, decision);
  } else {
    misc::fatal_error("Inlining_report.add_decision");  // (assert false)
  }
}

void print(Formatter& ppf, long depth, const Report& t) {
  for (const auto& [place, v] : t) {
    if (!v->is_call) {
      fprintf(ppf, "@[<h>%a Definition of %a%s@]@.", [&](Formatter& f) { print_stars(f, depth + 1); },
              [&](Formatter& f) { variable::print(f, place.cl); }, debuginfo::to_string(place.dbg));
      print(ppf, depth + 1, v->closure);
      if (depth == 0) ppf.print_newline();
      continue;
    }
    const Call& c = v->call;
    if (!c.decision) misc::fatal_error("Inlining_report.print: missing call decision");
    ppf.open_vbox(depth + 2);
    fprintf(ppf, "@[<h>%a Application of %a%s@]@;@;@[%a@]", [&](Formatter& f) { print_stars(f, depth + 1); },
            [&](Formatter& f) { variable::print(f, place.cl); }, debuginfo::to_string(place.dbg),
            [&](Formatter& f) { summary(f, *c.decision); });
    ppf.close_box();
    ppf.print_newline();
    ppf.print_newline();
    calculation(ppf, depth + 1, *c.decision);
    if (c.specialised) print(ppf, depth + 1, *c.specialised);
    if (c.inlined) print(ppf, depth + 1, *c.inlined);
    if (depth == 0) ppf.print_newline();
  }
}
}  // namespace

void record_decision(const Decision& decision, ClosureStack closure_stack) {
  if (!clflags::inlining_report) return;
  if (!is_call(closure_stack)) misc::fatal_error("record_decision: missing Call node");
  log().emplace_back(closure_stack, decision);
}

void save_then_forget_decisions(const std::string& output_prefix) {
  if (!clflags::inlining_report) return;
  // build: List.fold_left add_decision Place_map.empty log (the newest
  // decision first); each stack walked from its bottom (List.rev)
  Report report;
  for (std::size_t k = log().size(); k-- > 0;) {
    std::vector<ClosureStack> stack;
    for (ClosureStack n = log()[k].first; n; n = n->next) stack.insert(stack.begin(), n);
    add_decision(report, stack, 0, log()[k].second);
  }
  Formatter ppf;
  print(ppf, 0, report);

  std::ofstream out(output_prefix + ".inlining.org", std::ios::binary);
  out << ppf.contents();
  log().clear();
}

}  // namespace cppcaml::typing::inlining_stats
