(* TWO parameters write the result out once per parameter: an UNDER-count. *)
module Mk (X : sig type t end) (Z : sig type u end) :
  sig type k type 'a t val a : 'a t end =
struct type k = X.t type 'a t = 'a list let a = [] end
