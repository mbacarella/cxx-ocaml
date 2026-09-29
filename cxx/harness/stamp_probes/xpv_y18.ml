let f flag = let _ = match flag with `A -> 0 | `B r -> r in let flag2 = flag in
  let _ = match flag2 with `A -> succ | `B r -> r in ()
