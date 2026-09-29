module type E = sig end
open Ephemeron let x = K1.make let _ = (module K1 : E)
