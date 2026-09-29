module type S = sig val v : int module Q : sig val w : int end end
module Register (D : S) = struct let x = D.v end
module N = struct let v = 1 module Q = struct let w = 2 end end
module X = Register (N)
module Y = Register (N)
