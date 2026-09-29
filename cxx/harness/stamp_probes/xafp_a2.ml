module M : sig val f : int -> int end = struct let f x = x end
let v = M.f 0
