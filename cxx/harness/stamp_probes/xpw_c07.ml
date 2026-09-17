module type S = sig type t end
let f x = (x : (module S with type t = unit))
