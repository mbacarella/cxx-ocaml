(* The functor bound inside a MODULE. *)
module Outer = struct
  module Mk (X : sig type t end) :
    sig type k type 'a t val a : 'a t end =
  struct type k = X.t type 'a t = 'a list let a = [] end
end
