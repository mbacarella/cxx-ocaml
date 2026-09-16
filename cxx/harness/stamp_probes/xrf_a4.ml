type _ t = P : ('a * 'b) t
let f : type a. a t -> unit = function P -> ()
