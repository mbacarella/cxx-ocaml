exception E
exception F of int * string
exception G = Not_found
let f () = raise (F (1, ""))
