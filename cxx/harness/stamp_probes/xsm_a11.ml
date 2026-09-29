type z = int
module Typ : sig module type P = sig type t type t1 end end =
  struct module type P = sig type t type t1 end end
open Typ
module M = struct type t = int type t1 = int end
let p = (module M : P with type t = int)
