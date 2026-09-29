type z = int
module rec Typ : sig
  module type P = sig type t type t1 type t2 end
end = struct
  module type P = sig type t type t1 type t2 end
end
