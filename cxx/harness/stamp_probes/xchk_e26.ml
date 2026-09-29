module type S = sig
  module M : sig val s : unit end
 end
let f (x : (module S)) = let module X = (val x) in X.M.s
