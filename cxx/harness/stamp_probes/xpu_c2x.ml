module type E = sig end
open Ephemeron let _ = (module K1 : E) let _ = (module K1 : E)
  let _ = (module K1 : E)
