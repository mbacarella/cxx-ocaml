let f (flag : [< `A | `B of int]) = let _ = match flag with `A -> succ | `B r ->
  r in ()
