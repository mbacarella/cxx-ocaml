module type S = sig val v : int end
let f ((module X) : (module S)) = X.v
