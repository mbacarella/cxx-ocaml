type e = Z
module M = struct type nonrec e = A of e end
let k (M.A x) = x
