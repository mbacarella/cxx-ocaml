type z = int
module rec Typ : sig
  module type P = sig type t type t1 end  type u = A of (module P)
end = struct
  module type P = sig type t type t1 end  type u = A of (module P)
end
