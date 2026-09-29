(* a DOTTED label annotation keeps the parsetree's own strings (S566) *)
module N = struct type u = int end
type t = { s : N.u }
let f r = r.s
