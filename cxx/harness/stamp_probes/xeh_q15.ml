module type E = sig exception Ex end
let f (module M : E) (M.Ex | _) = "42"
