let ( let* ) o f = match o with None -> None | Some x -> f x
let ( and* ) a b = match a, b with Some x, Some y -> Some (x, y) | _ -> None
let r = let* x = Some 1 and* y = Some 2 in Some (x + y)
