module type S = sig val v : int end
module Register (D : S) = struct let x = D.v module Q = struct let w =
  2 end end
module N = struct let v = 1 end
module X = Register (N)
