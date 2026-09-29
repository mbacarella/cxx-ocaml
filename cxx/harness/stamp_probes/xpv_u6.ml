type t = [ `A ] let f flag = let _ = match flag with `A -> 0 | `B r -> r in let
  _ = match flag with `A -> succ | `B r -> r in let _ = match flag with #t -> 1
  in ()
