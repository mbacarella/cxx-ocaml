type z = int
module Typ : sig module type P = sig type t type t1 end end =
  struct module type P = sig type t type t1 end end
open Typ
let f (type s) x = let module M = (val x : P with type t = s) in ()
