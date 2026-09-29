module type Print = sig type t val print : t -> unit end
module PInt = struct type t = int let print = print_int end
let f (module P : Print) (x : P.t) = ()
let () = ignore f
