module type S = sig val v : int end
module Register (D:S) = struct let x = D.v end
module Simple = struct
module M = struct let v = 1 end
end
module EM = Register(Simple.M)
