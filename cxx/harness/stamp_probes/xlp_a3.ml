(* `'a t` named by no other item costs nothing. *)
module Mk (X : sig type t end) :
  sig type k type 'a t end =
struct type k = X.t type 'a t = 'a list end
