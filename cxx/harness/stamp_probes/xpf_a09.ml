module type ORD = sig type t end
module type SET = sig type t end
module B (F : sig module G : functor (X : ORD) -> SET end) = struct end
