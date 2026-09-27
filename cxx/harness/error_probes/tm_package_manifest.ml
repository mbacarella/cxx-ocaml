module type S = sig type t = int end
let f (x : (module S with type t = int)) = x
