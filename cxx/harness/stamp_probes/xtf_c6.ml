module type Print = sig type t val print : t -> unit end
let print (module P : Print) (x : P.t) = P.print x
module PList (P : Print) = struct type t = P.t list
  let rec aux = function [] -> () | [x] -> print (module P) x
    | hd :: tl -> print (module P) hd; aux tl  let print l = aux l end
module PInt = struct type t = int let print = print_int end
module PB = struct type t = bool let print = ignore end
let () = print (module PList(PInt)) [3; 1]; print (module PList(PB)) [true];
  print (module PInt) 3
