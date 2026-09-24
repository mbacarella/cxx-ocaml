(* a type declaration through Lazy.t loads CamlinternalLazy *)
type x = A of int Lazy.t
let z = 1
