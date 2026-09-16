module M = struct exception Ex end
type v = A | B | C
let f = function Some (M.Ex | _) | None -> 2
