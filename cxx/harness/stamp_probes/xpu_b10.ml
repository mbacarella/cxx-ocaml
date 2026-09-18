module type E = sig end
let _ = let module N = Int in (module N : E)
