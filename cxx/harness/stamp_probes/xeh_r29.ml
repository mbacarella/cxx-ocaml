module M = struct exception Ex end
type v = A | B | C
let f = function M.Ex, _ -> 1 | _, A -> 2 | _, (B | C) -> 3
