module type T = sig
  module P (B : sig end)
    (O : functor (S : sig type t end) -> sig type u end) : sig end
end
