module type E = sig end
let f () = let module N = Unit in (module N : E)
