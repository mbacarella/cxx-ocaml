type z = int
module type P = sig type t type t1 end
module Typ : sig end = struct type u = A of (module P) end
