module type E = sig exception Ex end
let f ((module M) : (module E)) = function M.Ex | _ -> "42"
