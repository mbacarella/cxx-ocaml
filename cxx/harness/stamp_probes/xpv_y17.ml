let f flag = let _ = match flag with `A -> 0 | `B r -> r in let _ = match Some
  flag with Some `A -> succ | Some (`B r) -> r | None -> succ in ()
