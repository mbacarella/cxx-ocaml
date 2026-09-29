module type Print = sig type t val print : t -> unit end
let f (module P : Print) = fun x -> P.print x
