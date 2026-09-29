let f flag = let _ = match flag with `A -> 0 | `B (Some r) -> r | `B None -> 1
  in let _ = match flag with `A -> succ | `B (Some r) -> r | `B None -> succ in
  ()
