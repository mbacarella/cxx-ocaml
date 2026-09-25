module M = struct module N = struct type e = A | B of e end end
let k = function M.N.B x -> x | _ -> M.N.A
