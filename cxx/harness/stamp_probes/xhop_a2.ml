module type T = functor (S : sig type t end) -> sig type u end
