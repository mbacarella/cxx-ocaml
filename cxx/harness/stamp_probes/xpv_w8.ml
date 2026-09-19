let f flag = let _ = match flag with `A -> 0 | `B (r, s) -> r + s in let _ =
  match flag with `A -> succ | `B (r, s) -> r in ()
