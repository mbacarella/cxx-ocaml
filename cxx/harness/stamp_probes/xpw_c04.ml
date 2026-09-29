module type S = sig type t type u end
let f (x : (module S with type t = unit and type u = int)) = ()
