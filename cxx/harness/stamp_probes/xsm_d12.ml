type z = int
module type P = sig type t type t1 end
module Typ : sig exception E of int end = struct exception E of int end
open Typ
let f = function E n -> n | _ -> 0
