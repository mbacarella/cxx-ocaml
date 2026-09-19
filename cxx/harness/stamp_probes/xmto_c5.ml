type z = int
module U = struct type t = int
  module Make (H : sig type t end) = struct type key = H.t end end
module U1 : sig include module type of U end = U
