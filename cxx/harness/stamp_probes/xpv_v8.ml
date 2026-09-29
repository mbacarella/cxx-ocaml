module M = struct let f (x : [< `A | `B of int]) = ignore x end let g x = M.f x;
  let _ = match x with `A -> succ | `B r -> r in ()
