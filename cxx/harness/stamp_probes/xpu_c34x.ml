module type E = sig end
open Float let _ = (module Float.Array : E)
