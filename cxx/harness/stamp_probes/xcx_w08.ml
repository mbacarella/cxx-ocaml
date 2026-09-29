type (_, _) t = A : (int, int) t
type _ q = Q : _ t q
let f (type a) (x : a q) = let Q = x in 0
