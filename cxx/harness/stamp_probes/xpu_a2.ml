module type T = sig type t end
let _ = (module Int : T)
