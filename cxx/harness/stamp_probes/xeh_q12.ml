module type E = sig exception Ex end
let f ((module M) : (module E)) ((M.Ex | _), (M.Ex | _)) = "42"
