(* an include of a dotted path splices its items *)
module X = struct module Y = struct type t = A | B let v = A end end
include X.Y
let w : t = B
