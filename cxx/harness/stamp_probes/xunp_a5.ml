module X = struct module type S = sig val v : int end end
let f (m : (module X.S)) = let module M = (val m) in M.v
