module type S = sig type t end
let f y = let g (x : (module S with type t = unit)) = () in g y
