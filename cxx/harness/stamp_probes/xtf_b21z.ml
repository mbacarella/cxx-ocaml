module type Print = sig type t val print : t -> unit end
let f (module P : Print) (x : int) = ()
module M : sig val g : (module Print) -> int -> unit end = struct let g = f end
