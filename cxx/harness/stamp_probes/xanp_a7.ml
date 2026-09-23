(* a local-open label annotation, read back out of the record *)
module N = struct type u = int end
type t = { s : N.(u) }
let f r = r.s
