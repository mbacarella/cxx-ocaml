module type S = sig type t end
let f (x : (module S with type t = unit)) (y : (module S with type t = int)) =
  ()
