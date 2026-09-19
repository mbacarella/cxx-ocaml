module M : sig type t val v : t end = struct type t = int let v = 0 end let f
  flag = let _ = match flag with `A -> 0 | `B r -> r in let _ = match flag with
  `A -> M.v | `B r -> r in ()
