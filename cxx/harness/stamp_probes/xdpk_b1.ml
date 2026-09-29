module type S = sig val v : int end
let f (module X : S) = X.v
