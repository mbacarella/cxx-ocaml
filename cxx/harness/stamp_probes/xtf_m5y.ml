module type Print = sig type t val print : t -> unit end
module PInt = struct type t = int let print = print_int end
module Id (P : Print) = struct type t = P.t let print = P.print end
module N = struct
  module PInt = struct type t = int let print = print_int end end
let f (module P : Print) (x : P.t) = ()
let () = f (module N.PInt) 3
