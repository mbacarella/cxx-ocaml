type z = int
module type P = sig type t type t1 end
module Typ : sig end = struct type t = ..
  type t += A of (module P with type t = int) end
