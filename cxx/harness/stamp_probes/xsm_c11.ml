type z = int
module type P = sig type t type t1 end
module rec Typ : sig
  module type Q = sig type u = A of (module P with type t = int) end end
  = struct
  module type Q = sig type u = A of (module P with type t = int) end end
