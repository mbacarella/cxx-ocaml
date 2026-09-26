let id x = x
let k x _ = x
let compose f g x = f (g x)
let twice f x = f (f x)
let a = twice (fun x -> x + 1) 3
