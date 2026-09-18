module A = struct type t = int end
module type S = sig type t end
let f (module X : S) = () let _ = f (module A)
