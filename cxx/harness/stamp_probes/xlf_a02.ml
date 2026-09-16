type 'a t
module type S = sig type data val key: data t end
module Register (D:S) = struct let key = D.key end
module M = struct type data = int let key : _ t = Obj.magic () end
module EM = Register(M)
