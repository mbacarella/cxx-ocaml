(* A VARIANT ARGUMENT names it. *)
module Mk (X : sig type t end) :
  sig type k type 'a t type u = A of int t end =
struct type k = X.t type 'a t = 'a list type u = A of int t end
