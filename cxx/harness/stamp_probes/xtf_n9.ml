module type Print = sig type t val print : t -> unit end
module PInt = struct type t = int let print = print_int end
module Id (P : Print) = struct type t = P.t let print = P.print end
let f (type a) (module P : Print with type t = a) (x : a) = ()
let () = f (module PInt) 3
