module N = struct let v = 1 module Q = struct let w = 2 module R =
  struct let z = 1 end end end
include N.Q
