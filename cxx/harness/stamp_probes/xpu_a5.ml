module type T = sig type t end
let _ = (module Int : T with type t = int)
let _ = (module Int : T with type t = int)
