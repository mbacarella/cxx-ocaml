type _ t = L : 'a list t | M : int t
let f : type a. a t -> int = fun x -> match x with M -> 0 | L -> 1
