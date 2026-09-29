module type ORD = sig type t end
module type SET = sig type t end
module B (F : functor (X : ORD) -> SET) (Elt : ORD) = struct
  module rec Z : SET = F(Elt)
end
