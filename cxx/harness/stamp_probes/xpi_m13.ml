module type T = sig type u end
module A = struct type t = int module M = struct type u = int end end
module type S = sig type t module M : T end
let f (type a) (module X : S with type t = a) = ()
let _ = f (module A)
