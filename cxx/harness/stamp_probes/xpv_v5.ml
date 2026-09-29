let f flag = let _ = match flag with `B r -> r | `A -> 0 in let _ = match flag
  with `A -> succ | `B r -> r in ()
