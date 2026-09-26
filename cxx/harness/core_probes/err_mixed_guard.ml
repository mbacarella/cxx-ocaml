let bad f = match f () with (Some _ | exception Not_found) when true -> 0 | _ -> 1
