module type Print = sig type t val print : t -> unit end
let f (module P : Print) (x : int) = ()
let g (module Q : Print) = f (module Q) 3
