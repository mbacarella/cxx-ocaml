module type Print = sig type t val print : t -> unit end
let f = fun (module P : Print) (x : P.t) -> ()
