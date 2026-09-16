module M = struct exception Ex end
type v = A | B | C
let f = function _ -> 2 | M.Ex -> 1
