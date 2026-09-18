module type Print = sig type t val print : t -> unit end
module type T = sig val f : (module Print) -> int -> unit end
