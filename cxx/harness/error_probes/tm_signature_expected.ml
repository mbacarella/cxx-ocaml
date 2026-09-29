module type S = functor (X : sig end) -> sig end
module type T = S with type t = int
