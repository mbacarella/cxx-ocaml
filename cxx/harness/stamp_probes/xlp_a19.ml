(* A `with` that hands the type a MANIFEST leaves nothing abstract. *)
module type Sml = sig type k type 'a t val a : 'a t end
module Mk (X : sig type t end) :
  Sml with type 'a t = 'a list =
struct type k = X.t type 'a t = 'a list let a = [] end
