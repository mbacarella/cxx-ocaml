let f x = match x with Some ((Not_found | _), (Exit | _)) -> 1 | None -> 2
