module type S = sig type t end
let _ = let module type T = S with type t = int in ()
