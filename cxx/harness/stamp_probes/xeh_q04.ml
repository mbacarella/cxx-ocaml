module type E = sig exception Ex end
let f ((module M) : (module E)) (_ : exn) = "42"
