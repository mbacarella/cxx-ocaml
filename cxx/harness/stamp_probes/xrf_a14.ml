type _ t = L : 'a list t
let f (type a) (x : a t) = match x with L -> ()
