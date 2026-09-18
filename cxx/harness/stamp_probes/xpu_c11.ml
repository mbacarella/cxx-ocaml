module type E = sig end
let f (x : (module E)) = x let _ = f (module Int : E)
