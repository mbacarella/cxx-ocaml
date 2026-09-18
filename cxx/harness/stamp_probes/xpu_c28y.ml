module type E = sig end
let _ = (module Ephemeron.K1 : E) let _ = (module Ephemeron.K1.Bucket : E)
