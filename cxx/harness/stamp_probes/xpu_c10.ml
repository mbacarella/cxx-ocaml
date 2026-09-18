module type E = sig end
let _ = [(module Int : E); (module Unit : E)]
