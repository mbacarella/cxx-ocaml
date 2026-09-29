type z = int
module Typ : sig module type P = sig type t type t1 end end =
  struct module type P = sig type t type t1 end end
open Typ
let f (type s) (x : (module P with type t = s)) = ()
