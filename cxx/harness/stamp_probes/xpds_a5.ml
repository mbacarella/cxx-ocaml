(* a nested chain: every component we added names a declaration *)
module A = struct module B = struct type t = C end end
let f = A.B.C
let g (x : A.B.t) = x
