module type S = sig val v : int end
module Register (D:S) = struct let x = 1 end
module M = struct let v = 1 end
module EM = Register(M)
