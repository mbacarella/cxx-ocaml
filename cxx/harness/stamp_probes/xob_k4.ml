module A = struct include Int end
open Set.Make(A)
let e = empty
