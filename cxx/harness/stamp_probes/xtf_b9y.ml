module type Print = sig type t val print : t -> unit end
let f : (module Print) -> int -> unit = fun (module P) x -> ()
