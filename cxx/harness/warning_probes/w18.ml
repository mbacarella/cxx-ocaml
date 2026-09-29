(* needs -principal: FLAGS=-principal *)
type t = {x : int}
type u = {x : float}
let g (r : t) = r.x
let h r = ignore (r : t); r.x
