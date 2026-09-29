module type E = sig end
module I = Int module J = I let _ = (module J : E)
