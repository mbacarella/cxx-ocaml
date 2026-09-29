(* A parameter NAMED by a module type is one ident less. *)
module type P = sig type t end
module Mk (X : P) : sig type k type 'a t val a : 'a t end =
struct type k = X.t type 'a t = 'a list let a = [] end
