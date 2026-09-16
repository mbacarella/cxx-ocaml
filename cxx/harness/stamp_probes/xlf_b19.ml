module type S = sig val v : int end
module Register (D : S) = struct let x = D.v end
module X = Register (struct let v = 1 end)
