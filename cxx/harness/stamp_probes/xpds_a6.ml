(* a record label reached through a local module *)
module R = struct type s = { f : int } end
let s = { R.f = 1 }
let g x = x.R.f
