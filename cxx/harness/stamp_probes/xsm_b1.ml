type z = int
module rec Typ : sig
  module type P = sig type t end
end = struct
  module type P = sig type t end
end
