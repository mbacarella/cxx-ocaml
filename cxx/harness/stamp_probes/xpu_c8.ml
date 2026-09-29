module type E = sig end
let _ = ((module Int) : (module E))
