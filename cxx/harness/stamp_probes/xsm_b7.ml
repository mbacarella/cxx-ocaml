type z = int
module rec Typ : sig
  module type P = sig type t type t1 end
end = struct
  module type P = sig type t type t1 end
end
and U : sig type t end = struct type t = int end
