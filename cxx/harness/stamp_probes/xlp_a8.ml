(* A GENERATIVE functor hands its result on unsubstituted. *)
module Mk () :
  sig type k type 'a t val a : 'a t end =
struct type k = int type 'a t = 'a list let a = [] end
