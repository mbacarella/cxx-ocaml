let f flag = let _ = match flag with `A -> 0 | `B r -> r | `C s -> s in let _ =
  match flag with `A -> succ | `B r -> r | `C s -> s in ()
