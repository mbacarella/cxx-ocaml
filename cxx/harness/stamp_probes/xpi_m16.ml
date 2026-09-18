module A = struct type t = int end
module type S = sig type t end
let f (type a) (module X : S with type t = a) = ()
let _ = let module B = struct type t = int end in f (module B)
