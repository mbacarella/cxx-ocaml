type z = int
module type P = sig type t type t1 end
module Typ : sig type 'a typ = Pair of (module P with type t = 'a) end =
  struct type 'a typ = Pair of (module P with type t = 'a) end
let f (type s) (p : (module P with type t = s)) = Typ.Pair p
