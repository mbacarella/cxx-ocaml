type z = int
module U = struct type t = int module type HT = sig type t end
  module Make (H : HT) = struct type key = H.t end end
