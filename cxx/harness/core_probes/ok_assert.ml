let f x = assert (x > 0); x
let g () = assert false
let h x : int = if x then assert false else 0
