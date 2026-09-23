(* a name resolved through an `open` is Pdot (root, <the lookup's own
   string>) -- one per lookup site, not one per declaration *)
open List
let f (x : int t) = x
let g (y : int t) = y
