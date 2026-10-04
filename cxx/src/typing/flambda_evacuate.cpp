// Evacuation of a flambda program (see flambda_evacuate.hpp).
#include "cppcaml/typing/flambda_evacuate.hpp"

namespace cppcaml::typing::flambda_evacuate {

using namespace flambda;

Program evacuate(const Program& program, const std::vector<const Zone*>& dying) {
  return Evacuator(dying).program(program);
}

}  // namespace cppcaml::typing::flambda_evacuate
