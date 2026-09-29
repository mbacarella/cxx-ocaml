module type Print = sig type t val print : t -> unit end
module M = struct let f (module P : Print) (x : P.t) = () end
