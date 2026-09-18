module A = struct type t = int module type T = sig type u end end
module type S = sig type t module type T = sig type u end end
let f (type a) (module X : S with type t = a) = ()
let _ = f (module A)
