module type ORD = sig type t end
module type SET = sig type t end
module F (X : ORD) : SET = struct type t = int end
module rec Elt : sig type t end = struct type t = int end
and Diet : SET = F(Elt)
