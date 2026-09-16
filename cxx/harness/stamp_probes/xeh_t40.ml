let f x = match x with Some (Not_found | _) | None -> 1 | _ -> 2
