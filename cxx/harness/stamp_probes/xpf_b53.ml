module type ORD = sig type t end
module type SET = sig type t end
module Elt : ORD = struct type t = int end
module B (F : functor (X : ORD) -> SET) = struct
  module Z = F(Elt)
end
