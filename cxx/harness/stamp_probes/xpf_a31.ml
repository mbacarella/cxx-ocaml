module type ORD = sig type t end
module type SET = sig type t end
module B (F : functor (X : ORD) -> functor (Y : ORD) -> SET) = struct end
