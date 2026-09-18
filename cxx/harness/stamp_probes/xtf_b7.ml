module type Print = sig type t val print : t -> unit end
let f (module P : Print) (x : P.t) = ()
module PInt = struct type t = int let print = print_int end
let () = f (module PInt) 3
