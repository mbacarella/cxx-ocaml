let f x = match x with Some (Not_found | _), Some (Exit | _) -> 1
  | None, _ -> 2 | _, None -> 3
