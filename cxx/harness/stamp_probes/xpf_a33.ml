module type ORD = sig type t end
module type SET = sig type t end
module B (Y : ORD) (F : functor (X : ORD) -> SET) = struct end
