module type Print = sig type t val print : t -> unit end
let print (module P : Print) (x : P.t) = P.print x
module PList (P : Print) = struct type t = P.t list
  let rec aux = function [] -> () | [x] -> print (module P) x
    | hd :: tl -> print (module P) hd; aux tl  let print l = aux l end
module PInt = struct type t = int let print = print_int end
module Q = PList(PInt)
type u = PList(PInt).t
