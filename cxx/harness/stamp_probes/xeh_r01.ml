module M = struct exception Ex end
type v = A | B | C
let f (M.Ex | _) = 1
