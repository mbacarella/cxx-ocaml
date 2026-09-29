type z = int
module U = struct type t = int
  module Make (H : sig type t end) = struct type key = H.t end end
module U1 : module type of U = U
module U2 : module type of U = U
