module M = struct exception Ex end
type v = A | B | C
let f x = try x () with M.Ex | _ -> 1
