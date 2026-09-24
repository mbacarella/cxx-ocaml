(* a functor is rebuilt by the nondep pass *)
include struct type t = int end
type t = float
module F (A : sig end) = struct type v = int end
