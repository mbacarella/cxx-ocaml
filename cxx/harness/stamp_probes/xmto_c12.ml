type z = int
module U = struct type t = int module type HT = sig type t end
  module type S = sig type key val x : int end
  module Make (H : HT) : S = struct type key = H.t let x = 1 end end
module U1 : sig include module type of U end = U
