let f flag = let _ = match (flag : [< `A | `B of int]) with `A -> succ | `B r ->
  r in ()
