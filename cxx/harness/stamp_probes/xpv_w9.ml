let f flag = let _ = match flag with `A -> 0 | `B r -> r in let _ = try match
  flag with `A -> succ | `B r -> r with Not_found -> succ in ()
