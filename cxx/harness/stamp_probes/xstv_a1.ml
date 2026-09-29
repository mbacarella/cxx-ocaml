module M = struct type e = A | B of e type f = C of e end
let k = function M.B x -> x | _ -> M.A
let j (M.C x) = x
