let f flag =
  let _ = match flag with `A -> succ | `B r -> r in
  ()
