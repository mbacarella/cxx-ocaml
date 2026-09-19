let f flag = let _ = match flag with `A -> 0 | `B r -> r in let _ = (function `A
  -> succ | `B r -> r) flag in ()
