module type Print = sig type t val print : t -> unit end
let f (module P : Print) (module Q : Print) (module R : Print)
    (x : P.t) (y : Q.t) (z : R.t) = ()
