type _ t = O : < m : int; .. > t
let f : type a. a t -> unit = function O -> ()
