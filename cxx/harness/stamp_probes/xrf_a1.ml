type _ t = L : 'a list t
let f : type a. a t -> a = function L -> []
