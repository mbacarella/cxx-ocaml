module X = struct module type S = sig type t end end
let f x = let g (x : (module X.S with type t = unit)) = () in g x
