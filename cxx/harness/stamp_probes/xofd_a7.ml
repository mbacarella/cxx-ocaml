type r = {mutable a : int; b : bool}
module M = struct let v {a; _} = a let u x = x.a <- 3 end
