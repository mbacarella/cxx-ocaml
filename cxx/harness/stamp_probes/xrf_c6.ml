type (_, _) t = R : ('a, 'a list) t
let f : type a b. (a, b) t -> unit = function R -> ()
