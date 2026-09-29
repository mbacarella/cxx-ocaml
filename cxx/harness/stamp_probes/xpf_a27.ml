module type ORD = sig type t end
module type SET = sig type t end
module B (F : functor (X : ORD) -> SET) = struct end
module C = B
