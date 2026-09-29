(* the nondep pass rebuilds a module after the shadow *)
include struct type t = int end
type t = float
module Y = struct type v = int end
