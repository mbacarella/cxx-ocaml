module type S = sig type t end
module Register (D:S) = struct let x = 1 end
module M = struct type t = int end
module EM = Register(M)
