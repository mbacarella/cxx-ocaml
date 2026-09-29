type e = Z
module M = struct type e = A | B of e end
let k = function M.B x -> x | _ -> M.A
