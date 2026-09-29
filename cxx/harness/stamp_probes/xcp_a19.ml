module M : sig
  module F : functor (X : sig type t end) -> sig type u end
  type z = Zed
end = struct
  module F (X : sig type t end) = struct type u = X.t end
  type z = Zed
end
type w = Wed
