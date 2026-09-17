module type ORD = sig type t end
module type SET = sig type t end
module B (F : functor (X : sig type t end) -> SET) = struct end
