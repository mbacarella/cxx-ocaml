type e = A | B of e
let k = function B x -> x | _ -> A
