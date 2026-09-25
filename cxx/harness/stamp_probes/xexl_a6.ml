exception E of {x : int; y : int}
exception G = E
let f = function E {x; _} -> x | _ -> 0
