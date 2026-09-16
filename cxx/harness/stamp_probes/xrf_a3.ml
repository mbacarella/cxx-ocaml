type _ t = V : [> `A] t
let f : type a. a t -> unit = function V -> ()
