// Port of middle_end/flambda/closure_offsets.ml (see closure_offsets.hpp).
#include "cppcaml/typing/closure_offsets.hpp"

#include "cppcaml/typing/flambda_utils.hpp"
#include "cppcaml/typing/misc.hpp"

namespace cppcaml::typing::closure_offsets {

using namespace flambda;

namespace {

Result add_closure_offsets(Result r, const SetOfClosures* set) {
  // Build the table mapping the functions declared by the set of closures
  // to the positions of their individual "infix" closures inside the
  // runtime closure block.  (All of the environment entries will come
  // afterwards.)
  long env_pos = -1;
  variable::Map<long> function_offsets = r.function_offsets;
  set->function_decls->funs.iter([&](variable::t id, const FunctionDeclaration* function_decl) {
    long pos = env_pos + 1;
    long arity = flambda_utils::function_arity(function_decl);
    env_pos = env_pos + 1  // GC header; either [Closure_tag] or [Infix_tag]
              + 1          // full application code pointer
              + 1          // arity
              + (arity > 1 ? 1 : 0);  // partial application code pointer
    if (function_offsets.mem(id))
      misc::fatal_error("Closure_offsets.add_closure_offsets: function offset for " + variable::unique_name(id) +
                        " would be defined multiple times");
    function_offsets = function_offsets.add(id, pos);
  });
  // Adds the mapping of free variables to their offset.
  long pos = env_pos;
  variable::Map<long> free_variable_offsets = r.free_variable_offsets;
  set->free_vars.iter([&](variable::t var, const SpecialisedTo&) {
    if (free_variable_offsets.mem(var))
      misc::fatal_error("Closure_offsets.add_closure_offsets: free variable offset for " + variable::unique_name(var) +
                        " would be defined multiple times");
    free_variable_offsets = free_variable_offsets.add(var, pos);
    pos = pos + 1;
  });
  return {function_offsets, free_variable_offsets};
}

}  // namespace

Result compute(const Program& program) {
  Result r;
  for (const SetOfClosures* set : flambda_utils::all_sets_of_closures(program)) r = add_closure_offsets(r, set);
  return r;
}

}  // namespace cppcaml::typing::closure_offsets
