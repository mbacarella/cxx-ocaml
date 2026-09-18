module type E = sig end
let _ = let open Ephemeron in (module K1 : E)
