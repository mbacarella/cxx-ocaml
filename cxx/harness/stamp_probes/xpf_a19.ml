module type ORD = sig type t end
module type SET = sig type t end
module M : sig module B (F : functor (X : ORD) -> SET) : sig end end = struct
  module B (F : functor (X : ORD) -> SET) = struct end
end
