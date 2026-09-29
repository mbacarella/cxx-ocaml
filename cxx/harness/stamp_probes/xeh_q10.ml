module type E = sig exception Ex end
let f ((module M) : (module E)) (e : exn) = match e with M.Ex | _ -> "42"
