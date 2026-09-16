let f x = match x with exception (Not_found | _) -> 1 | _ -> 2
