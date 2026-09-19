type t = [ `A | `B of string ] let f flag = let _ = match flag with `A -> 0 | `B
  r -> r in let _ = match flag with #t -> 2 in let _ = match flag with `A ->
  succ | `B r -> r in ()
