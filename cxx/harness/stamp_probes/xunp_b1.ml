module type S = sig type t val v : t end
let f m = let module M = (val m : S with type t = int) in M.v
