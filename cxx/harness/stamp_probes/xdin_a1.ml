(* an include of a dotted path splices its items *)
module X = struct module Y = struct let v = 1 end end
module M = struct include X.Y end
