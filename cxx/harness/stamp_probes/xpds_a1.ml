(* a path's component is the name object of the declaration it names *)
module N = struct type t = A | B end
let g N.(A | B) = ()
let i = N.A
