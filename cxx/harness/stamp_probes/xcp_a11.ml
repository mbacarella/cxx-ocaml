module M : sig
  module F : functor (X : sig type t end) -> sig type u end
end = struct
  module F (X : sig type t end) = struct type u = X.t end
end
type x = Xed
