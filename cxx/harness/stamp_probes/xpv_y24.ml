let f flag = let _ = match flag with `A -> 0 | _ -> 1 in let _ = match flag with
  `A -> succ | `B r -> r in ()
