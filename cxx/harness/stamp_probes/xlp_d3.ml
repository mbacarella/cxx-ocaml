(* An ascription inside an EXPRESSION is not charged. *)
module Mk (X : sig type t end) :
  sig type k type 'a t val a : 'a t end =
struct type k = X.t type 'a t = 'a list let a = [] end
module X : sig type t end = struct type t = int end
let f () =
  let module S : sig type k type 'a t val a : 'a t end = Mk (X) in
  ignore S.a
