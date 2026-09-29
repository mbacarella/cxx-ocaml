module type ORD = sig type t end
module type SET = sig type t end
module F (X : ORD) : SET = struct type t = int end
