let f flag = let _ = match flag with `A -> 0 | `B r -> r in let `B r = flag in
  ignore (r : string)
