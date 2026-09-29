module type Print = sig type t val print : t -> unit end
module PInt = struct type t = int let print = print_int end
let f (module P : Print) (module Q : Print) (x : P.t) (y : Q.t) = ()
let () = f (module PInt) (module PInt) 3 3
