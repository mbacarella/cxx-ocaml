type _ t = L : 'a list t | M : int t
let f : type a. a t option -> int = function Some M -> 0 | None -> 1
