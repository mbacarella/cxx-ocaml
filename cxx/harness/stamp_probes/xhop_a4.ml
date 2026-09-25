module type T = functor (S : sig type t end) -> sig type u end
module P (B : sig end) (O : T) = struct end
