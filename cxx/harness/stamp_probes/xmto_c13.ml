type z = int
module U = struct type t = int
  module Make (H : sig type t end) = struct type key = H.t end end
module type T = module type of U
module type T2 = module type of U
