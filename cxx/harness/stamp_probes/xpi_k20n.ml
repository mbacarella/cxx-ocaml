module A = struct module M = struct type t = int end end
module type S = sig type t end
let f (module X : S) = ()
let _ = f (module A.M)
