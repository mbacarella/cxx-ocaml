module type ORD = sig type t end
module type SET = sig type t end
module type T = sig module B (F : functor (X : ORD) -> SET) : sig end end
