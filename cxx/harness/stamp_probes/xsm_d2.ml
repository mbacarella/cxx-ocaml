type z = int
module type P = sig type t type t1 end
module rec Typ : sig type 'a typ = Pair of (module P with type t = 'a) end =
  struct type 'a typ = Pair of (module P with type t = 'a) end
open Typ
let f (type s) (p : (module P with type t = s)) = Pair p
