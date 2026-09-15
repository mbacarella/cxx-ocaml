(* No result signature: nothing is written out. *)
module Mk (X : sig type t end) =
struct type k = X.t type 'a t = 'a list let a = [] end
