module type Print = sig type t val print : t -> unit end
let f : (module P : Print) -> P.t -> unit = fun (module P) x -> ()
