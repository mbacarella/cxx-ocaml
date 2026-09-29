type _ t = L : 'a list t
let f : type a. a t option -> int = function None -> 1 | Some L -> 0
