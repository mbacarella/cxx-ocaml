let f flag flag2 = let _ = match flag2 with `A -> 0 | `B r -> r in let _ = match
  flag, flag2 with | `A, `A -> succ | `A, `B r -> r | `B, `A -> succ | `B, `B r
  -> r in let _ = match flag2, 0 with `A, _ -> 1 in ()
