let f flag = let _ = match flag with `A -> 0 | `B r -> r in let _ = match flag
  with `A -> 1 | `B r -> r + 1 in ()
