(* an include of a dotted path splices its items *)
(* control: an undotted local include *)
module X = struct type t = A let v = A end
include X
