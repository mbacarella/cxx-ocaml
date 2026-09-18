module type E = sig end
let x = (module Int : E) let y = (module Int : E)
