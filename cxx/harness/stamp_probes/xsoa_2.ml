module type S = sig open Set.Make(Bool) type u = t end
module type T = sig type t val v : t end
let g (x : Set.Make(Bool).t) = x
