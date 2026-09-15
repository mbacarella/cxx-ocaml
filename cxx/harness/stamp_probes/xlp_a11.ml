(* TWO parameterized abstract types are worth one write-out, not two. *)
module Mk (X : sig type t end) : sig
  type k type 'a t type 'a u val a : 'a t val b : 'a u end =
struct type k = X.t type 'a t = 'a list type 'a u = 'a list
  let a = [] let b = [] end
