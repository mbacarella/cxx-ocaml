let g : [< `A | `B of int] -> int -> int = function `A -> succ | `B r -> r
