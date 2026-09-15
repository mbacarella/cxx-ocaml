(* A functor whose written-out result leaves `'a t` abstract and named. *)
module Mk (X : sig type t end) :
  sig type k type 'a t val a : 'a t end =
struct type k = X.t type 'a t = 'a list let a = [] end
