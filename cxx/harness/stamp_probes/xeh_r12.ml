module M = struct exception Ex end
type v = A | B | C
let f = function _, (M.Ex | _) -> 1
