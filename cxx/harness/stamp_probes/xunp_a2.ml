module type S = sig type t val v : t end
let f (m : (module S with type t = int)) = let module M = (val m) in M.v
