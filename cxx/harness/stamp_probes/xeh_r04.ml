module M = struct exception Ex end
type v = A | B | C
let f (A | B) = 1
