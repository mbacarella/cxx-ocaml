module type E = sig end
module _ = (Int : E) let _ = (module Int : E)
