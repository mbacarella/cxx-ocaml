module type Print = sig type t val print : t -> unit end
let f (x : (module Print)) (y : int) = ()
let g (module Q : Print) = f (module Q) 3
