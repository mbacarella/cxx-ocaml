type z = int
module rec Typ : sig
  module type P = sig type t end  module type P2 = P
end = struct
  module type P = sig type t end  module type P2 = P
end
