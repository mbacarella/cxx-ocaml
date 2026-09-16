type 'a t
module type S = sig val key: int t end
module Register (D:S) = struct let key = D.key end
module M = struct let key : _ t = Obj.magic () end
