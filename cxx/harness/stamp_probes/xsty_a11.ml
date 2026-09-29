(* known miss: a class the nondep pass rebuilds *)
include struct type t = int end
type t = float
class c = object end
