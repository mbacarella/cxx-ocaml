module type Print = sig type t val print : t -> unit end
module PInt = struct type t = int let print = print_int end
module Id (P : Print) = struct type t = P.t let print = P.print end
let f (module P : Print) ((x, y) : P.t * int) = ()
let () = f (module Id(PInt)) (3, 4)
