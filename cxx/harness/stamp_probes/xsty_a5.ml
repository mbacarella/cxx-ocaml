(* a module type is rebuilt by the nondep pass *)
include struct type t = int end
type t = float
module type S = sig type v end
