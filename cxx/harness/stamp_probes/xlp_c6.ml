(* A NAMED parameter type, declared and ascribed: one ident less apiece. *)
module type P = sig type t end
module Mk (X : P) : sig type k type 'a t val a : 'a t end =
struct type k = X.t type 'a t = 'a list let a = [] end
module X : P = struct type t = int end
module S : sig type k type 'a t val a : 'a t end = Mk (X)
