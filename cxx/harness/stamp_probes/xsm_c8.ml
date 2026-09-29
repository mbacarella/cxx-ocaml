type z = int
module type P = sig type t type t1 end
module rec Typ : sig type u = A of (module P with type t = int) end
  = struct type u = A of (module P with type t = int) end
