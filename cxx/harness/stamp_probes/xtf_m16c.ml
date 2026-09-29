module type Print = sig type t val print : t -> unit end
module PInt = struct type t = int let print = print_int end
module Id (P : Print) = struct type t = P.t let print = P.print end
module N = struct let f (module P : Print) (x : P.t) = () end
let () = N.f (module PInt) 3; N.f (module PInt) 4
