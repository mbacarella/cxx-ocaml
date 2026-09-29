module N = struct let v = 1 module Q = struct let w = 2 module R =
  struct let z = 1 end end end
module F (X : sig val w : int end) = struct end
module Y = F (N.Q)
module Z = F (N.Q)
