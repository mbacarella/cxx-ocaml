(* Five items, to weigh the result flat. *)
module Mk (X : sig type t end) : sig
  type k type 'a t val a : 'a t val b : 'a t val c : 'a t end =
struct type k = X.t type 'a t = 'a list
  let a = [] let b = [] let c = [] end
