module type Print = sig type t val print : t -> unit end
module M : sig val f : (module P : Print) -> int -> unit end =
  struct let f (module P : Print) (x : int) = () end
