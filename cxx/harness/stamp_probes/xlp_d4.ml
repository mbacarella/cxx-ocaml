(* The functor bound inside another FUNCTOR: an under-count. *)
module Outer (Z : sig type u end) = struct
  module Mk (X : sig type t end) :
    sig type k type 'a t val a : 'a t end =
  struct type k = X.t type 'a t = 'a list let a = [] end
end
