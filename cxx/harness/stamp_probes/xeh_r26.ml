module M = struct exception Ex end
type v = A | B | C
let f = function M.Ex | Not_found -> 1
