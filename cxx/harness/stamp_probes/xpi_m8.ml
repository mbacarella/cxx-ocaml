module A = struct type t = int end
module type S = sig type t end
let _ = let (module X : S with type t = int) = (module A) in ()
