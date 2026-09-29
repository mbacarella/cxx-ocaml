type 'a t = [< `A | `B of int ] as 'a let f (flag : 'a t) = let _ = match flag
  with `A -> succ | `B r -> r in ()
