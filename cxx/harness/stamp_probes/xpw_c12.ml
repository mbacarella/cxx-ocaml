module type S = sig type t end
let f = fun (x : (module S with type t = unit)) -> ()
