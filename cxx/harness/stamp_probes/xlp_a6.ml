(* The result a bare NAME is left a path. *)
module type Sml = sig type k type 'a t val a : 'a t end
module Mk (X : sig type t end) : Sml =
struct type k = X.t type 'a t = 'a list let a = [] end
