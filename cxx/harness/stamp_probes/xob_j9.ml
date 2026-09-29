module A = struct type t = int let compare = compare end
open Set.Make(A)
let x = 1
