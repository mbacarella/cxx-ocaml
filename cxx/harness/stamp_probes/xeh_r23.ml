module M = struct exception Ex end
type v = A | B | C
let f = function M.Ex -> 1 | M.Ex | _ -> 2
