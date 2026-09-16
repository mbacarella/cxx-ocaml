module type E = sig exception Ex end
let f ((module M) : (module E)) = function M.Ex -> "42" | _ -> "1"
