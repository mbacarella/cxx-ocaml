module N = struct module type S = sig type t end end
module A = struct type t = int end
let f (type a) (module X : N.S with type t = a) = ()
