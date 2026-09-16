type 'a t
module type S = sig val key: int t end
module Register (D:S) = struct let x = 1 end
module M = struct let key : _ t = Obj.magic () end
module EM = Register(M)
