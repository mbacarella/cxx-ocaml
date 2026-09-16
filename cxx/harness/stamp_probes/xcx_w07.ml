type (_, _) t = A : (int, int) t
type 'a p = P : _ t -> 'a p
let f (type a) (x : a p) = let P _ = x in 0
