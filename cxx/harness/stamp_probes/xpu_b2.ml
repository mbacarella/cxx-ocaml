module type E = sig end
let _ = (module Stdlib.Int : E)
