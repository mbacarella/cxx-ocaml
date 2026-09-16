let f x = match x with (Not_found | _) :: _ -> 1 | [] -> 2
