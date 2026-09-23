(* a parameterised field at two instances of the one annotation *)
type 'a t = { c : 'a }
let f (r : int t) = r.c
let g (r : string t) = r.c
