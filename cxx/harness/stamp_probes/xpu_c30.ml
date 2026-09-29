module type E = sig end
let f (module X : E) = (module X : E) let _ = f (module Unit : E)
