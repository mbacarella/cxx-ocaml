// Port of asmcomp/selectgen.ml, asmcomp/amd64/selection.ml,
// asmcomp/polling.ml and asmcomp/dataflow.ml.  See selection.hpp.
#include "cppcaml/typing/selection.hpp"

#include <algorithm>
#include <functional>
#include <map>
#include <memory>
#include <stdexcept>
#include <unordered_map>

#include "cppcaml/typing/clflags.hpp"
#include "cppcaml/typing/location.hpp"

#include "selectgen.hpp"

namespace cppcaml::typing::selection {


void reset() { current_function_name = {}; }

}  // namespace cppcaml::typing::selection

// ---- Polling (with Dataflow.Backward) --------------------------------------------------------
namespace cppcaml::typing::polling {

using namespace mach;
using IK = Instruction::K;
using MK = mach::Operation::K;

namespace {

bool starts_with(std::string_view s, std::string_view p) { return s.substr(0, p.size()) == p; }
bool function_is_assumed_to_never_poll(std::string_view func) {
  return starts_with(func, "caml_apply") || starts_with(func, "caml_send");
}

// Dataflow.Backward over a two-point domain: bot, join, lessequal
struct Domain {
  bool (*join)(bool, bool);
  bool (*lessequal)(bool, bool);
  bool bot;
};
using Transfer = std::function<bool(Instr, bool next, bool exn)>;

std::pair<bool, std::map<long, bool>> analyze(const Domain& D, bool exnescape, const Transfer& transfer, Instr instr) {
  std::map<long, bool> lbls;
  auto get_lbl = [&](long n) {
    auto it = lbls.find(n);
    return it == lbls.end() ? D.bot : it->second;
  };
  std::function<bool(bool, bool, Instr)> before = [&](bool end_, bool exn, Instr i) -> bool {
    switch (i->desc) {
      case IK::Iend: return transfer(i, end_, exn);
      case IK::Ireturn: return transfer(i, D.bot, D.bot);
      case IK::Iop: {
        if (i->op->k == MK::Itailcall_ind || i->op->k == MK::Itailcall_imm) return transfer(i, D.bot, D.bot);
        bool bx = before(end_, exn, i->next);
        return transfer(i, bx, exn);
      }
      case IK::Iifthenelse: {
        bool bx = before(end_, exn, i->next);
        bool b1 = before(bx, exn, i->ifso);
        bool b0 = before(bx, exn, i->ifnot);
        return transfer(i, D.join(b1, b0), exn);
      }
      case IK::Iswitch: {
        bool bx = before(end_, exn, i->next);
        bool b1 = D.bot;
        for (Instr c : i->cases) b1 = D.join(b1, before(bx, exn, c));
        return transfer(i, b1, exn);
      }
      case IK::Icatch: {
        bool bx = before(end_, exn, i->next);
        if (i->rec == cmm::RecFlag::Nonrecursive) {
          for (auto& h : i->handlers) lbls[h.n] = before(bx, exn, h.body);
        } else {
          for (;;) {
            bool changed = false;
            for (auto& h : i->handlers) {
              bool b0 = get_lbl(h.n);
              bool b1 = before(bx, exn, h.body);
              if (!D.lessequal(b1, b0)) {
                lbls[h.n] = b1;
                changed = true;
              }
            }
            if (!changed) break;
          }
        }
        bool b = before(bx, exn, i->body);
        return transfer(i, b, exn);
      }
      case IK::Iexit: return transfer(i, get_lbl(i->nfail), exn);
      case IK::Itrywith: {
        bool bx = before(end_, exn, i->next);
        bool bh = before(bx, exn, i->ifnot);
        bool bb = before(bx, bh, i->ifso);
        return transfer(i, bb, exn);
      }
      case IK::Iraise: return transfer(i, D.bot, exn);
    }
    return D.bot;
  };
  bool b = before(D.bot, exnescape, instr);
  return {b, lbls};
}

// unsafe_or_safe: Safe = true (bot = Unsafe)
const Domain unsafe_or_safe{[](bool a, bool b) { return a && b; }, [](bool a, bool b) { return !a || b; }, false};
// polls_before_prtc: Always_polls = true (bot = Always_polls)
const Domain polls_before_prtc{[](bool a, bool b) { return a && b; }, [](bool a, bool b) { return !(b && !a); },
                               true};

std::map<long, bool> polled_loops_analysis(Instr funbody) {
  Transfer transfer = [](Instr i, bool next, bool exn) -> bool {
    switch (i->desc) {
      case IK::Iend: return next;
      case IK::Iop:
        if (i->op->k == MK::Ialloc || i->op->k == MK::Ipoll || i->op->k == MK::Itailcall_ind ||
            i->op->k == MK::Itailcall_imm)
          return true;
        if (operation_can_raise(*i->op)) return next && exn;
        return next;
      case IK::Ireturn: return true;
      case IK::Iraise: return exn;
      default: return next;
    }
  };
  // [exnescape] is [Safe] because we can't loop infinitely having returned
  // from the function via an unhandled exception.
  return analyze(unsafe_or_safe, true, transfer, funbody).second;
}

bool potentially_recursive_tailcall_always_polls(const selection::FuncNames& future_funcnames, Instr funbody) {
  Transfer transfer = [&](Instr i, bool next, bool exn) -> bool {
    switch (i->desc) {
      case IK::Iend: return next;
      case IK::Iop:
        if (i->op->k == MK::Ialloc || i->op->k == MK::Ipoll) return true;
        if (i->op->k == MK::Itailcall_ind) return false;  // this is a PTRC
        if (i->op->k == MK::Itailcall_imm)
          return !(future_funcnames.count(i->op->func) || function_is_assumed_to_never_poll(i->op->func));
        if (operation_can_raise(*i->op)) return next && exn;
        return next;
      case IK::Ireturn: return true;
      case IK::Iraise: return exn;
      default: return next;
    }
  };
  return analyze(polls_before_prtc, polls_before_prtc.bot, transfer, funbody).first;
}

Instr add_poll(Instr i) { return instr_cons_debug(iop(mach::mop(MK::Ipoll)), {}, {}, i->dbg, i); }

Instr instr_body(const std::map<long, bool>& handler_safe, Instr i0) {
  auto safe = [&](long k) {
    auto it = handler_safe.find(k);
    return it == handler_safe.end() ? false : it->second;
  };
  std::function<Instr(const std::set<long>&, Instr)> instr = [&](const std::set<long>& ube, Instr i) -> Instr {
    switch (i->desc) {
      case IK::Iifthenelse: {
        // { i with desc = ..; next = .. }: right to left
        Instr next = instr(ube, i->next);
        Instr i1 = instr(ube, i->ifnot);
        Instr i0 = instr(ube, i->ifso);
        Instr c = copy(i);
        c->ifso = i0;
        c->ifnot = i1;
        c->next = next;
        return c;
      }
      case IK::Iswitch: {
        Instr next = instr(ube, i->next);
        std::vector<Instr> cases;
        for (Instr k : i->cases) cases.push_back(instr(ube, k));
        Instr c = copy(i);
        c->cases = cases;
        c->next = next;
        return c;
      }
      case IK::Icatch: {
        std::set<long> ube2 = ube;
        if (i->rec == cmm::RecFlag::Recursive)
          for (auto& h : i->handlers)
            if (!safe(h.n)) ube2.insert(h.n);
        // Since we are only interested in unguarded _back_ edges, we don't
        // use [ube'] for instrumenting [body], but just [ube] instead.
        Instr body = instr(ube, i->body);
        Instr next = instr(ube, i->next);
        std::vector<Handler> hs;
        for (auto& h : i->handlers) hs.push_back({h.n, instr(ube2, h.body)});
        Instr c = copy(i);
        c->handlers = hs;
        c->body = body;
        c->next = next;
        return c;
      }
      case IK::Iexit:
        if (ube.count(i->nfail)) return add_poll(i);
        return i;
      case IK::Itrywith: {
        Instr next = instr(ube, i->next);
        Instr hdl = instr(ube, i->ifnot);
        Instr body = instr(ube, i->ifso);
        Instr c = copy(i);
        c->ifso = body;
        c->ifnot = hdl;
        c->next = next;
        return c;
      }
      case IK::Iend:
      case IK::Ireturn:
      case IK::Iraise: return i;
      case IK::Iop: {
        Instr c = copy(i);
        c->next = instr(ube, i->next);
        return c;
      }
    }
    return i;
  };
  return instr({}, i0);
}

}  // namespace

mach::Fundecl instrument_fundecl(const mach::Fundecl& f) {
  if (function_is_assumed_to_never_poll(f.fun_name)) return f;
  std::map<long, bool> handler_needs_poll = polled_loops_analysis(f.fun_body);
  Instr new_body = instr_body(handler_needs_poll, f.fun_body);
  if (f.fun_poll == lambda::PollAttribute::Error_poll) {
    // find_poll_alloc_or_calls new_body
    PollError err;
    instr_iter(
        [&](Instr i) {
          if (i->desc != IK::Iop) return;
          switch (i->op->k) {
            case MK::Ipoll: err.points.push_back({PollError::Point::Poll, i->dbg}); break;
            case MK::Ialloc: err.points.push_back({PollError::Point::Alloc, i->dbg}); break;
            case MK::Icall_ind:
            case MK::Icall_imm:
            case MK::Itailcall_ind:
            case MK::Itailcall_imm: err.points.push_back({PollError::Point::Function_call, i->dbg}); break;
            case MK::Iextcall:
              if (i->op->alloc) err.points.push_back({PollError::Point::External_call, i->dbg});
              break;
            default: break;
          }
        },
        new_body);
    if (!err.points.empty()) throw err;
  }
  mach::Fundecl r = f;
  r.fun_body = new_body;
  return r;
}

void report_error(format_doc::Formatter& ppf, const PollError& e) {
  using P = PollError::Point;
  long num_inserted_polls = 0;
  for (auto& [p, _] : e.points)
    if (p == P::Poll) ++num_inserted_polls;
  long num_user_polls = static_cast<long>(e.points.size()) - num_inserted_polls;
  if (num_user_polls == 0) {
    format_doc::fprintf(ppf,
                        "Function with poll-error attribute contains polling points (inserted by the compiler)\n");
    return;
  }
  format_doc::fprintf(ppf, "Function with poll-error attribute contains polling points:\n");
  for (auto& [p, dbg] : e.points) {
    if (p == P::Poll) continue;
    const char* t = p == P::Alloc ? "allocation" : p == P::Function_call ? "function call" : "external call that allocates";
    format_doc::fprintf(ppf, "\t%s at ", t);
    location::doc::loc(ppf, debuginfo::to_location(dbg));
    format_doc::fprintf(ppf, "\n");
  }
  if (num_inserted_polls > 0)
    format_doc::fprintf(ppf,
                        "\t(plus compiler-inserted polling point(s) in prologue and/or loop back edges)\n");
}

bool requires_prologue_poll(const selection::FuncNames& future_funcnames, std::string_view fun_name, Instr i) {
  if (function_is_assumed_to_never_poll(fun_name)) return false;
  return !potentially_recursive_tailcall_always_polls(future_funcnames, i);
}

}  // namespace cppcaml::typing::polling
