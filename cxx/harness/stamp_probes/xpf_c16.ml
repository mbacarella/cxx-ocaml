module type ORD = sig type t val compare : t -> t -> int end
module type SET = sig type t end
module B (F : functor (X : ORD) -> SET) = struct
  module rec Elt : sig type t val compare : t -> t -> int end =
    struct type t = int let compare _ _ = 0 end
  and Diet : SET = F(Elt)
end
