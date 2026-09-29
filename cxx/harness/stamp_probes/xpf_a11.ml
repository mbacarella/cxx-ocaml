module type ORD = sig type t end
module type SET = sig type t end
module type FT = functor (X : ORD) -> SET
