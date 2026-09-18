module A = struct type t = int end
module type S = sig type t end
module B = (A : S with type t = int)
let f (type a) (module X : S with type t = a) = ()
