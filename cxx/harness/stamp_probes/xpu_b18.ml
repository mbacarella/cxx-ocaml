module type E = sig end
let _ = (module Int : E) let x = Int.zero
