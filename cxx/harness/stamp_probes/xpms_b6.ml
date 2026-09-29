module type P = sig type r val f : r -> r end
module F (D : sig type t end) = struct
  module type Q = P with type r := D.t
end
