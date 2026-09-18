module type Print = sig type t val print : t -> unit end
let print (module P : Print) x = P.print x
