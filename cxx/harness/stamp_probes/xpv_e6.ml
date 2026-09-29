let f flag = let _ = match flag with `A -> 0 | `B r -> r in let _ = match flag
  with `B r -> r | `A -> succ in ()
