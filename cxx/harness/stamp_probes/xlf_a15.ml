module Simple = struct
module type S = sig val v : int end
module Register (D:S) = struct let x = D.v end
end
module M = struct let v = 1 end
module EM = Simple.Register(M)
