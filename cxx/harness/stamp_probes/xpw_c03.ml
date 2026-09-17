module type S = sig type t end
let f (x : (module S with type t = unit)) = ()
let g (x : (module S with type t = unit)) = ()
