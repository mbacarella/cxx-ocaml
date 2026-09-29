type (_, _) t = A : (int, int) t
type p = P : (_, int) t -> p
let f (x : p) = let P _ = x in 0
