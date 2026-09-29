let f x = match x with Some (Some (Not_found | _)) -> 1 | Some None -> 2
  | None -> 3
