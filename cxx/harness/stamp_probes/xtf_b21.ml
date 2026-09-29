module type Print = sig type t val print : t -> unit end
let f (module P : Print) (x : P.t) = ()
module M : sig val g : (module P : Print) -> P.t -> unit end =
  struct let g = f end
