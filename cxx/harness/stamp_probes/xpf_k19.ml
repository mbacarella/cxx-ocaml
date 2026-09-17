module type ORD = sig type t end
module type SET = sig type t end
module type FT = functor (X : ORD) -> SET
module M : sig module B (F : FT) : sig end end = struct
  module B (F : functor (X : ORD) -> SET) = struct end
end
