class type c = object method m : int end
type _ t = K : #c t
let f : type a. a t -> unit = function K -> ()
