module type E = sig exception Ex end
let f ((module M) : (module E)) (Not_found) = "42"
