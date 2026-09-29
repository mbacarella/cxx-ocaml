let f x = match x with Some (Not_found | _) -> 1 | Some _ -> 2 | None -> 3
