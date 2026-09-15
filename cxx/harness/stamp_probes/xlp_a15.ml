(* A GADT EXCEPTION names it. *)
module Mk (X : sig type t end) : sig
  type k type 'a t exception E : 'a t -> exn end =
struct type k = X.t type 'a t = 'a list exception E : 'a t -> exn end
