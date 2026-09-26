let bad = let f (type t) (x : t) = x in f
let r : int = bad "a"
