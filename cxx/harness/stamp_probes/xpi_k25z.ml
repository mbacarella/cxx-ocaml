module A = struct type t = int end
module type S = sig type t end
let f (type a) (y : int) (module X : S with type t = a) = ()
