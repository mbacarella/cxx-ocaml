module A = struct type t = int type u = int end
module type S = sig type t type u end
let f (type a) (module X : S with type t = a and type u = int) = ()
