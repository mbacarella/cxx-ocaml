(* Arity TWO is as good as arity one. *)
module Mk (X : sig type t end) :
  sig type k type ('a, 'b) t val a : (int, 'a) t end =
struct type k = X.t type ('a, 'b) t = 'a list let a = [] end
