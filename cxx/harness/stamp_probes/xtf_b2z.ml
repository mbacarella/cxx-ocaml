module type Print = sig type t val print : t -> unit end
module type T = sig val f : (module P : Print) -> int -> unit end
