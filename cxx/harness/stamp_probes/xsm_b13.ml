type z = int
module rec Typ : sig
  module type P = sig type t type t1 end
end = struct
  module type P = sig type t type t1 end
end
type u = (module Typ.P with type t = int)
