module A = struct type t = int end
module type S = sig type t end
let f (type a) (module X : S with type t = a) = ()
let _ = f (module A)
let _ = f (module A)
