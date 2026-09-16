type _ t = O : < m : int; .. > t | Q : int t
let f : type a. a t -> unit = function Q -> () | O -> ()
