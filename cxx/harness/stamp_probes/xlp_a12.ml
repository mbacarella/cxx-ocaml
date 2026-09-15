(* The type and its user both inside a SUBMODULE of the result. *)
module Mk (X : sig type t end) :
  sig module M : sig type 'a t val a : 'a t end end =
struct module M = struct type 'a t = 'a list let a = [] end end
