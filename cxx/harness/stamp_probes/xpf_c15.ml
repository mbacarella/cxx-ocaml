module type ORD = sig type t end
module type SET = sig type t end
module B (F : functor (X : ORD) -> SET) = struct
  module rec Elt : sig type t end = struct type t = int end
  and Diet : sig type t end = F(Elt)
end
