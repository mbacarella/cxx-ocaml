module type ORD = sig type t end
module type SET = sig type t end
module A (Y : ORD) = struct
  module B (F : functor (X : ORD) -> SET) = struct end
end
