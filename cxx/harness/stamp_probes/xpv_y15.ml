let f flag = let _ = match flag with `A -> 0 | `B r -> r | `C -> 1 in let _ =
  match flag with `A -> succ | `B r -> r | `C -> succ in ()
