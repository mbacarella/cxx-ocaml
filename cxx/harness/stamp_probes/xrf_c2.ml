type _ t = C : 'a -> 'a t
let f (x : int t) = match x with C x -> x
