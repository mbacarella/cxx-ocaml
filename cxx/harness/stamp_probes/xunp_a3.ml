module X = struct module type S = sig type t val v : t end end
let f (m : (module X.S with type t = int)) = let module M = (val m) in M.v
