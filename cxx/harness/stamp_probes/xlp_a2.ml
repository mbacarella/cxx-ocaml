(* The MONO twin: `type t` takes no parameters and costs nothing. *)
module Mk (X : sig type t end) :
  sig type k type t val a : t end =
struct type k = X.t type t = int let a = 0 end
