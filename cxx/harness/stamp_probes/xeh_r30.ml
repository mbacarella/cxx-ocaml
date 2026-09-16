module M = struct exception Ex end
type v = A | B | C
let f = function Some (M.Ex | _) -> 1 | _ -> 2
