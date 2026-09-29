let f x = match x with Some (Some y) -> y + 1 | _ -> 0
let bad = f (Some (Some "x"))
