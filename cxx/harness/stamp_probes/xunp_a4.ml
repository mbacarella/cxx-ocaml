module X = struct module type S = sig type t val v : t end end
let f m = let module M = (val m : X.S with type t = int) in M.v
