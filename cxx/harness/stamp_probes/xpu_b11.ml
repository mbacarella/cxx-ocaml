module type E = sig end
let f () = (module Int : E)
