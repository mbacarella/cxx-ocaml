type _ t = V : [ `A | `B] t
let f : type a. a t -> unit = function V -> ()
