(* the control: a LOCAL module's member strings are S566's law, untouched *)
module M = struct type t = int end
let f (x : M.t) = x
let g (y : M.t) = y
