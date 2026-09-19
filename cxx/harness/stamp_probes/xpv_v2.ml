let f flag = let _ = match flag with `A -> [] | `B r -> r in let _ = match flag
  with `A -> [""] | `B r -> r in ()
