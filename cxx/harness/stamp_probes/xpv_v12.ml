let f flag = let _ = match flag with `A -> 0 | `B (`X r) -> r in let _ = match
  flag with `A -> succ | `B (`X r) -> r in ()
