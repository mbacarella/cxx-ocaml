module type T = sig type t end
module _ = (Int : T with type t = int)
let _ = (module Int : T with type t = int)
