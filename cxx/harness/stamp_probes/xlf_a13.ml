module Simple = struct
module type S = sig type t end
module Register (D:S) = struct let x = 1 end
module M = struct type t = int end
end
module EM = Simple.Register(Simple.M)
