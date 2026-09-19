type z = int
module U = struct type t = int
  module Make (H : sig type t end) = struct type key = H.t type u = int end end
module type T = module type of U
