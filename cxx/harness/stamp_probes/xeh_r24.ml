module M = struct exception Ex end
type v = A | B | C
let f x = match x with M.Ex | _ -> 1
