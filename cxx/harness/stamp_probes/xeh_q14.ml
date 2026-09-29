module type E = sig exception Ex end
let f ((module M) : (module E)) (None | _) = "42"
