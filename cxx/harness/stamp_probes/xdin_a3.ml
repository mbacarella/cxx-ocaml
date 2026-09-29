(* an include of a dotted path splices its items *)
module X = struct module Y = struct type t let f (x : t) = x end end
module M = struct include X.Y let g = f end
