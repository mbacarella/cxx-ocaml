let f flag = let _ = match flag with `A -> 0 | `B -> 1 in let _ = match
  flag with `A -> 0 | `B -> 1 | `C -> 2 in ()
