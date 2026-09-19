type z = int
module type P = sig type t type t1 end
module Typ : sig type 'a typ = Pair of (module P with type t = 'a) end =
  struct type 'a typ = Pair of (module P with type t = 'a) end
open Typ
let f (type s) (t : s typ) = match t with
  | Pair p -> let module M = (val p : P with type t = s) in ()
