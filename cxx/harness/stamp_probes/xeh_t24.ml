let f x = match x with [Not_found | _] -> 1 | [] -> 2 | _ :: _ :: _ -> 3
