module type ORD = sig type t end
module type SET = sig type t end
module B (F : ORD -> SET) = struct end
