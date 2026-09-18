module type Print = sig type t val print : t -> unit end
module PInt = struct type t = int let print = print_int end
module Id (P : Print) = struct type t = P.t let print = P.print end
let f (module P : Print) (module Q : Print) (x : P.t) (y : Q.t) = ()
let () = f (module Id(PInt)) (module Id(PInt)) 3 3
