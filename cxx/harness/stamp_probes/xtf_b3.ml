module type Print = sig type t val print : t -> unit end
module M : sig val f : (module P : Print) -> P.t -> unit end =
  struct let f (module P : Print) (x : P.t) = () end
