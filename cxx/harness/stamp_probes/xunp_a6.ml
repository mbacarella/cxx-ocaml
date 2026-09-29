module type S = sig type t val v : t val w : t -> string end
let f (m : (module S)) = let module M = (val m) in M.w M.v
