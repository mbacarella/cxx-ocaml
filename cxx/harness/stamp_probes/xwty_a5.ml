(* Lazy.t expands to CamlinternalLazy.t, which loads it *)
module type S = sig val x : int Lazy.t end
let z = 1
