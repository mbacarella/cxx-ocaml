let g flag = (match flag with `B r -> r + 0 | `A -> 0), (match flag with
  `B r -> [r] | `A -> [])
