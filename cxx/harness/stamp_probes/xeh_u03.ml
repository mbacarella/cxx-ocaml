let ( let* ) x f = f x
let g x = let* (Not_found | _) = x in 1
