module type ORD = sig type t end
module type SET = sig type t end
module B (M : sig module F : functor (X : ORD) -> SET end) = struct
  module rec Elt : sig type t end = struct type t = int end
  and Diet : SET = M.F(Elt)
end
