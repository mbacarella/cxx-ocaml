module type S = sig module Data: sig type t end end
module Register (D:S) = struct let x = 1 end
module M = struct module Data = struct type t = int end end
module EM = Register(M)
