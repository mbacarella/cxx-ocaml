module A = struct type t = int type u = int let v = 1 end
module type S = sig type t end
let f (type a) (module X : S with type t = a) = ()
let _ = f (module A)
