let f x = match x with (Not_found | _) :: (Exit | _) :: _ -> 1 | _ -> 2
