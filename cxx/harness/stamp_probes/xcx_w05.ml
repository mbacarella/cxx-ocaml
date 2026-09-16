type (_, _, _) t = A : (int, int, int) t
type p = P : _ t -> p
let f (x : p) = let P _ = x in 0
