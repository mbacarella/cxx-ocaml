type t = [ `A | `B of string ] let f flag = let _ = match flag with `A -> 0 | `B
  r -> r in let _ = match flag with `B _ -> 1 | #t -> 2 in ()
