(* THREE parameters, the same under-count again. *)
module Mk (X : sig type t end) (Z : sig type u end)
          (W : sig type w end) :
  sig type k type 'a t val a : 'a t end =
struct type k = X.t type 'a t = 'a list let a = [] end
