let f flag = let _ = match flag with `A -> 0 | `B r -> r in let _ = match flag
  with `A -> succ | (`B r as x) -> ignore x; r in ()
