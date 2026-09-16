module N = struct let v = 1 module Q = struct let w = 2 module R =
  struct let z = 1 end end end
module Y = (N.Q : sig val w : int end)
