let f flag x = let _ = match flag, x with `A, _ -> 0 | `B r, _ -> r in let _ =
  match flag, x with `A, _ -> succ | `B r, _ -> r in ()
