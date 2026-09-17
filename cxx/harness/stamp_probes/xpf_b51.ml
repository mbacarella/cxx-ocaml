module type ORD = sig type t end
module type SET = sig type t end
module B (F : functor (X : ORD) -> SET) = struct
  module Elt : ORD = struct type t = int end
  module Z : SET = F(Elt)
end
