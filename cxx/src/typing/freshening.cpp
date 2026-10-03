// Port of middle_end/flambda/freshening.ml (see freshening.hpp).
#include "cppcaml/typing/freshening.hpp"

#include "cppcaml/typing/misc.hpp"

namespace cppcaml::typing::freshening {

using format::Formatter;
using format::fprintf;

namespace project_var {
namespace {
// Variable.Map.print Variable.print (Identifiable.Make_map.print)
void print_map(Formatter& ppf, const variable::Map<variable::t>& m) {
  auto elts = [&](Formatter& f) {
    m.iter([&](variable::t id, variable::t v) {
      fprintf(f, "@ (@[%a@ %a@])", pr(variable::print, id), pr(variable::print, v));
    });
  };
  fprintf(ppf, "@[<1>{@[%a@ @]}@]", elts);
}

// Compose (T).compose ~earlier ~later
variable::Map<variable::t> compose_map(const variable::Map<variable::t>& earlier,
                                       const variable::Map<variable::t>& later) {
  if (variable::Map<variable::t>::equal([](variable::t a, variable::t b) { return variable::equal(a, b); }, earlier,
                                        later) ||
      later.is_empty())
    return earlier;
  return earlier.mapi([&](variable::t src_var, variable::t var) -> variable::t {
    if (later.mem(src_var)) {
      Formatter f;
      fprintf(f, "Freshening.Project_var.compose: domains of substitutions must be disjoint.  earlier=%a later=%a",
              pr(print_map, earlier), pr(print_map, later));
      misc::fatal_error(f.contents());
    }
    if (const variable::t* v = later.find_opt(var)) return *v;
    return var;
  });
}
}  // namespace

void print(Formatter& ppf, const T& t) {
  fprintf(ppf, "{ vars_within_closure %a, closure_id %a }", pr(print_map, t.vars_within_closure),
          pr(print_map, t.closure_id));
}

variable::t apply_closure_id(const T& t, variable::t closure_id) {
  if (const variable::t* v = t.closure_id.find_opt(closure_id)) return *v;
  return closure_id;
}

variable::t apply_var_within_closure(const T& t, variable::t var_in_closure) {
  if (const variable::t* v = t.vars_within_closure.find_opt(var_in_closure)) return *v;
  return var_in_closure;
}

T compose(const T& earlier, const T& later) {
  // (the record's fields right to left: closure_id first)
  variable::Map<variable::t> closure_id = compose_map(earlier.closure_id, later.closure_id);
  variable::Map<variable::t> vars = compose_map(earlier.vars_within_closure, later.vars_within_closure);
  return {vars, closure_id};
}
}  // namespace project_var

}  // namespace cppcaml::typing::freshening
