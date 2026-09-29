let f flag = let _ = match flag with `A -> 0 | `B r -> r in let _ = match flag
  with `A -> succ | `B r -> r in let g x = match x with `A -> 0 | `B r -> r in
  ignore (g flag)
