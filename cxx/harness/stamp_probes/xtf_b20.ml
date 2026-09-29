module type Print = sig type t val print : t -> unit end
let f (module P : Print) (x : P.t) = ()
module type T = module type of struct let g = f end
