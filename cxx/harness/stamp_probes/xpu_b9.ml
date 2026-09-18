module type E = sig end
let _ = (module Set.Make(Int) : E)
