(* The result a `with` over a name: the signature is written out. *)
module type Sml = sig type k type 'a t val a : 'a t end
module Mk (X : sig type t end) : Sml with type k = X.t =
struct type k = X.t type 'a t = 'a list let a = [] end
