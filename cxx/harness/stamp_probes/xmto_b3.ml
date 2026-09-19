type z = int
module U = struct type t = int let x = 0
  module type HT = sig type t val equal : t -> t -> bool end
  module Make (H : HT) = struct type key = H.t type u = H.t let y = 1 end end
